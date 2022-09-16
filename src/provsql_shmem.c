#include "math.h"

#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "storage/shmem.h"
#include "storage/fd.h"
#include "utils/array.h"
#include "utils/datum.h"
#include "utils/hsearch.h"
#include "utils/uuid.h"
#include "executor/spi.h"


#include "unistd.h"

#include "provsql_shmem.h"

#define PROVSQL_DUMP_FILE "provsql.tmp"

shmem_startup_hook_type prev_shmem_startup = NULL;
int provsql_init_nb_gates;
int provsql_max_nb_gates;
int provsql_avg_nb_wires;

static void provsql_shmem_shutdown(int code, Datum arg);

provsqlSharedState *provsql_shared_state = NULL;
HTAB *provsql_hash = NULL;

void provsql_shmem_startup(void)
{
  bool found;
  HASHCTL info;

  if(prev_shmem_startup)
    prev_shmem_startup();

  // Reset in case of restart
  provsql_shared_state = NULL;
  provsql_hash = NULL;


  LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

  provsql_shared_state = ShmemInitStruct(
      "provsql",
      add_size(offsetof(provsqlSharedState, wires),
      mul_size(sizeof(pg_uuid_t), provsql_max_nb_gates * provsql_avg_nb_wires)),
      &found);

  if(!found) {
#if PG_VERSION_NUM >= 90600
    /* Named lock tranches were added in version 9.6 of PostgreSQL */
    provsql_shared_state->lock =&(GetNamedLWLockTranche("provsql"))->lock;
#else
    provsql_shared_state->lock =LWLockAssign();
#endif /* PG_VERSION_NUM >= 90600 */
    provsql_shared_state->nb_wires=0;
  }

  memset(&info, 0, sizeof(info));
  info.keysize = sizeof(pg_uuid_t);
  info.entrysize = sizeof(provsqlHashEntry);

  provsql_hash = ShmemInitHash(
      "provsql hash",
      provsql_init_nb_gates,
      provsql_max_nb_gates,
      &info,
      HASH_ELEM | HASH_BLOBS
  );

  LWLockRelease(AddinShmemInitLock);

  // If we are in the main process, we set up a shutdown hook
  if(!IsUnderPostmaster)
    on_shmem_exit(provsql_shmem_shutdown, (Datum) 0);

  // Already initialized
  if(found)
    return;


  if( access( "provsql.tmp", F_OK ) == 0 ) {
    switch (provsql_deserialize("provsql.tmp"))
    {
    case 1:
      //elog(ERROR, "Error while opening the file during deserialization");
      break;
    
    case 2:
      //elog(ERROR, "Error while reading the file during deserialization");
      break;
    
    case 3:
      elog(ERROR, "Error while closing the file during deserialization");
      break;
    }
  } 

}

static void provsql_shmem_shutdown(int code, Datum arg)
{

  #if PG_VERSION_NUM >= 90600
    // Named lock tranches were added in version 9.6 of PostgreSQL
    provsql_shared_state->lock =&(GetNamedLWLockTranche("provsql"))->lock;
  #else
    provsql_shared_state->lock =LWLockAssign();
  #endif // PG_VERSION_NUM >= 90600

  switch (provsql_serialize("provsql.tmp"))
  {
  case 1:
    elog(INFO, "Error while opening the file during serialization");
    break;
  
  case 2:
    elog(INFO, "Error while writing to file during serialization");
    break;
  
  case 3:
    elog(INFO, "Error while closing the file during serialization");
    break;
  }

  LWLockRelease(provsql_shared_state->lock);


  // TODO (void) durable_rename(PROVSQL_DUMP_FILE ".tmp", PROVSQL_DUMP_FILE, LOG);

}




Size provsql_memsize(void)
{
  Size size = 0;

  // Size of the shared state structure
  size = add_size(size, offsetof(provsqlSharedState, wires));
  // Size of the array of wire ends
  size = add_size(size, mul_size(sizeof(pg_uuid_t), provsql_max_nb_gates * provsql_avg_nb_wires));
  // Size of the hashtable of gates
  size = add_size(size, 
                  hash_estimate_size(provsql_max_nb_gates, sizeof(provsqlHashEntry)));

  return size;
}

PG_FUNCTION_INFO_V1(create_gate_shmem);
Datum create_gate_shmem(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  gate_type type = (gate_type) PG_GETARG_INT32(1);
  ArrayType *children = PG_ARGISNULL(2)?NULL:PG_GETARG_ARRAYTYPE_P(2);
  int nb_children = 0;
  provsqlHashEntry *entry;
  bool found;

  if(PG_ARGISNULL(0) || PG_ARGISNULL(1))
    elog(ERROR, "Invalid NULL value passed to create_gate");

  if(children) {
   if(ARR_NDIM(children) > 1)
     elog(ERROR, "Invalid multi-dimensional array passed to create_gate");
   else if(ARR_NDIM(children) == 1)
     nb_children = *ARR_DIMS(children);
  }

  LWLockAcquire(provsql_shared_state->lock, LW_EXCLUSIVE);

  if(false && hash_get_num_entries(provsql_hash) == provsql_max_nb_gates) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Too many gates in in-memory circuit");
    //TODO instead, call a function to save it on disk.
  }

  if(nb_children && provsql_shared_state->nb_wires + nb_children > provsql_max_nb_gates * provsql_avg_nb_wires) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Too many wires in in-memory circuit");
  }

  entry = (provsqlHashEntry *) hash_search(provsql_hash, token, HASH_ENTER, &found);

  if(!found) {
    constants_t constants=initialize_constants();

    entry->type = -1;
    for(int i=0; i<nb_gate_types; ++i) {
      if(constants.GATE_TYPE_TO_OID[i]==type) {
        entry->type = i;
        break;
      }
    }
    if(entry->type == -1)
      elog(ERROR, "Invalid gate type");

    entry->nb_children = nb_children;
    entry->children_idx = provsql_shared_state->nb_wires;
    
    if(nb_children) {
      pg_uuid_t *data = (pg_uuid_t*) ARR_DATA_PTR(children);

      for(int i=0; i<nb_children; ++i) {
        provsql_shared_state->wires[entry->children_idx + i] = data[i];
      }

      provsql_shared_state->nb_wires += nb_children;
    }

    if(entry->type == gate_zero)
      entry->prob = 0.;
    else if(entry->type == gate_one)
      entry->prob = 1.;
    else
      entry->prob = NAN;

    entry->info1 = entry->info2 = 0;
  }

  LWLockRelease(provsql_shared_state->lock);
  
  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(set_prob_shmem);
Datum set_prob_shmem(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  double prob = PG_GETARG_FLOAT8(1);
  provsqlHashEntry *entry;
  bool found;

  if(PG_ARGISNULL(0) || PG_ARGISNULL(1))
    elog(ERROR, "Invalid NULL value passed to set_prob_shmem");

  LWLockAcquire(provsql_shared_state->lock, LW_EXCLUSIVE);

  entry = (provsqlHashEntry *) hash_search(provsql_hash, token, HASH_ENTER, &found);

  if(!found) {
    hash_search(provsql_hash, token, HASH_REMOVE, &found);
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Unknown gate");
  }
  
  if(entry->type != gate_input && entry->type != gate_mulinput) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Probability can only be assigned to input token");
  }

  entry->prob = prob;

  LWLockRelease(provsql_shared_state->lock);
  
  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(set_infos);
Datum set_infos(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  unsigned info1 = PG_GETARG_INT32(1);
  unsigned info2 = PG_GETARG_INT32(2);
  provsqlHashEntry *entry;
  bool found;

  if(PG_ARGISNULL(0) || PG_ARGISNULL(1))
    elog(ERROR, "Invalid NULL value passed to set_infos");

  LWLockAcquire(provsql_shared_state->lock, LW_EXCLUSIVE);

  entry = (provsqlHashEntry *) hash_search(provsql_hash, token, HASH_ENTER, &found);

  if(!found) {
    hash_search(provsql_hash, token, HASH_REMOVE, &found);
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Unknown gate");
  }

  if(entry->type == gate_eq && PG_ARGISNULL(2)) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Invalid NULL value passed to set_infos");
  }

  if(entry->type != gate_eq && entry->type != gate_mulinput) {
    LWLockRelease(provsql_shared_state->lock);
    elog(ERROR, "Infos cannot be assigned to this gate type");
  }

  entry->info1 = info1;
  if(entry->type == gate_eq)
    entry->info2 = info2;

  LWLockRelease(provsql_shared_state->lock);
  
  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(get_gate_type_shmem);
Datum get_gate_type_shmem(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  provsqlHashEntry *entry;
  bool found;
  gate_type result = -1;

  if(PG_ARGISNULL(0))
    PG_RETURN_NULL();

  LWLockAcquire(provsql_shared_state->lock, LW_SHARED);

  entry = (provsqlHashEntry *) hash_search(provsql_hash, token, HASH_FIND, &found);
  if(found)
    result = entry->type;
  
  LWLockRelease(provsql_shared_state->lock);

  if(!found)
    PG_RETURN_NULL();
  else {
    constants_t constants=initialize_constants();
    PG_RETURN_INT32(constants.GATE_TYPE_TO_OID[result]);
  }
}

PG_FUNCTION_INFO_V1(get_children_shmem);
Datum get_children_shmem(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  provsqlHashEntry *entry;
  bool found;
  ArrayType *result = NULL;

  if(PG_ARGISNULL(0))
    PG_RETURN_NULL();

  LWLockAcquire(provsql_shared_state->lock, LW_SHARED);

  entry = (provsqlHashEntry *) hash_search(provsql_hash, token, HASH_FIND, &found);
  if(found) {
    Datum *children_ptr = palloc(entry->nb_children * sizeof(Datum));
    constants_t constants=initialize_constants();
    for(int i=0;i<entry->nb_children;++i) {
      children_ptr[i] = UUIDPGetDatum(&provsql_shared_state->wires[entry->children_idx + i]);
    }
    result = construct_array( //TODO UTiliser cette façon de faire dans la version disque
        children_ptr,
        entry->nb_children,
        constants.OID_TYPE_PROVENANCE_TOKEN,
        16,
        false,
        'c');
    pfree(children_ptr);
  }
  
  LWLockRelease(provsql_shared_state->lock);

  if(!found)
    PG_RETURN_NULL();
  else
    PG_RETURN_ARRAYTYPE_P(result);
}

PG_FUNCTION_INFO_V1(get_prob_shmem);
Datum get_prob_shmem(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  provsqlHashEntry *entry;
  bool found;
  double result = NAN;

  if(PG_ARGISNULL(0))
    PG_RETURN_NULL();

  LWLockAcquire(provsql_shared_state->lock, LW_SHARED);

  entry = (provsqlHashEntry *) hash_search(provsql_hash, token, HASH_FIND, &found);
  if(found)
    result = entry->prob;
  
  LWLockRelease(provsql_shared_state->lock);

  if(isnan(result))
    PG_RETURN_NULL();
  else
    PG_RETURN_FLOAT8(result);
}

PG_FUNCTION_INFO_V1(get_infos);
Datum get_infos(PG_FUNCTION_ARGS)
{
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  provsqlHashEntry *entry;
  bool found;
  unsigned info1 =0, info2 = 0;

  if(PG_ARGISNULL(0))
    PG_RETURN_NULL();

  LWLockAcquire(provsql_shared_state->lock, LW_SHARED);

  entry = (provsqlHashEntry *) hash_search(provsql_hash, token, HASH_FIND, &found);
  if(found) {
    info1 = entry->info1;
    info2 = entry->info2;
  }
  
  LWLockRelease(provsql_shared_state->lock);

  if(info1 == 0)
    PG_RETURN_NULL();
  else {
    TupleDesc tupdesc;
    Datum values[2];
    bool nulls[2];

    get_call_result_type(fcinfo,NULL,&tupdesc);
    tupdesc = BlessTupleDesc(tupdesc);

    nulls[0] = false;
    values[0] = Int32GetDatum(info1);
    if(entry->type == gate_eq) {
      nulls[1] = false;
      values[1] = Int32GetDatum(info2);
    } else
      nulls[1] = (entry->type != gate_eq);

    PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
  }
}

PG_FUNCTION_INFO_V1(create_gate);
Datum create_gate(PG_FUNCTION_ARGS){
  constants_t constants = initialize_constants();

  /*
  HeapTuple entry;
  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  gate_type type = (gate_type) PG_GETARG_INT32(1);
  ArrayType *children = PG_ARGISNULL(2)?NULL:PG_GETARG_ARRAYTYPE_P(2);
  */

  Datum arguments[3]={datumCopy(PG_GETARG_DATUM(0), false, UUID_LEN), PG_GETARG_DATUM(1), PG_ARGISNULL(2)?0:datumCopy(PG_GETARG_DATUM(2), false, -1)};
  Oid argtypes[3]={ constants.OID_TYPE_PROVENANCE_TOKEN,
                    constants.OID_TYPE_GATE_TYPE,
                    constants.OID_TYPE_UUID_ARRAY
                  };
  char nulls[3] = {' ',' ',PG_ARGISNULL(2)?'n':' '};


  if(PG_ARGISNULL(0) || PG_ARGISNULL(1))
    elog(ERROR, "Invalid NULL value passed to create_gate");
/*
  if(children) {
   if(ARR_NDIM(children) > 1)
     elog(ERROR, "Invalid multi-dimensional array passed to create_gate");
   else if(ARR_NDIM(children) == 1)
     nb_children = *ARR_DIMS(children);
  }
*/


  SPI_connect();
  if (false && hash_get_num_entries(provsql_hash) >= provsql_max_nb_gates){
    if (SPI_execute_with_args(
            "SELECT provsql.create_gate_disk ($1,$2,$3) ", 
            3,argtypes,arguments,nulls, false, 0
    ) != SPI_OK_SELECT){
      elog(ERROR, "Something wrong happened while trying to create the gate");
    }
  }
  else {
    if (SPI_execute_with_args(
            "SELECT provsql.create_gate_shmem ($1,$2,$3) ", 
            3,argtypes,arguments,nulls, false, 0
    ) != SPI_OK_SELECT){
      elog(ERROR, "Something wrong happened while trying to create the gate");
    }
  }

  SPI_finish(); 
  PG_RETURN_VOID();
}


PG_FUNCTION_INFO_V1(get_gate_type);
Datum get_gate_type(PG_FUNCTION_ARGS){
  //pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  HeapTuple entry;
  bool isnull;
  Datum result = -1;
  constants_t constants = initialize_constants();

  Datum arguments[1]={datumCopy(PG_GETARG_DATUM(0),false,16)};
  Oid argtypes[1]={constants.OID_TYPE_PROVENANCE_TOKEN};
  char nulls[1] = {' '};


  SPI_connect();
  if (false && hash_get_num_entries(provsql_hash) >= provsql_max_nb_gates){
    if (SPI_execute_with_args(
            "SELECT provsql.get_gate_type_disk ($1) ", 
            1,argtypes,arguments,nulls, false, 0
    ) != SPI_OK_SELECT){
      elog(ERROR, "Something wrong happened while trying to retrieve the gate");
    }
  }
  else {
    if (SPI_execute_with_args(
            "SELECT provsql.get_gate_type_shmem ($1) ", 
            1,argtypes,arguments,nulls, false, 0
    ) != SPI_OK_SELECT){
      elog(ERROR, "Something wrong happened while trying to retrieve the gate");
    }
  }
  entry = SPI_copytuple(SPI_tuptable->vals[0]);
  result = heap_getattr(entry, 1, SPI_tuptable->tupdesc, &isnull);
  SPI_finish();
  if (!isnull)
  {
    PG_RETURN_DATUM(result);
  }
  else {
    PG_RETURN_VOID();
  }
}

PG_FUNCTION_INFO_V1(set_prob);
  Datum set_prob(PG_FUNCTION_ARGS){
 // HeapTuple entry;
//  pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
 // double prob = PG_GETARG_FLOAT8(1);
  constants_t constants = initialize_constants();

  Datum arguments[2]={datumCopy(PG_GETARG_DATUM(0), false, 16), PG_GETARG_DATUM(1)};
  Oid argtypes[2]={constants.OID_TYPE_PROVENANCE_TOKEN, constants.OID_TYPE_FLOAT };
  char nulls[2] = {' ',' '};

  SPI_connect();
  if(PG_ARGISNULL(0) || PG_ARGISNULL(1))
  elog(ERROR, "Invalid NULL value passed to set_prob");
  //TODO Fix the bug before then remove the "true" in the condition
  if (false && hash_get_num_entries(provsql_hash) >= provsql_max_nb_gates){

    if (SPI_execute_with_args(
            "SELECT provsql.set_prob_disk ($1, $2) ", 
            2,argtypes,arguments,nulls, false, 0
    ) != SPI_OK_SELECT){
      elog(ERROR, "Something wrong happened while trying to retrieve the gate");
    }
  }
  else {
    if (SPI_execute_with_args(
            "SELECT provsql.set_prob_shmem ($1, $2) ", 
            2,argtypes,arguments,nulls, false, 0
    ) != SPI_OK_SELECT){
      elog(ERROR, "Something wrong happened while trying to retrieve the gate");
    }
  }
  SPI_finish();


  PG_RETURN_VOID();
}




PG_FUNCTION_INFO_V1(get_prob);
  Datum get_prob(PG_FUNCTION_ARGS){
  HeapTuple entry;
  constants_t constants = initialize_constants();
  
  //pg_uuid_t *token = DatumGetUUIDP(PG_GETARG_DATUM(0));
  bool isnull; 
  Datum result;
  Datum arguments[1]={datumCopy(PG_GETARG_DATUM(0), false, 16)};
  Oid argtypes[1]={constants.OID_TYPE_PROVENANCE_TOKEN};
  char nulls[1] = {' '};

  SPI_connect();
  if (false && hash_get_num_entries(provsql_hash) >= provsql_max_nb_gates){
    if (SPI_execute_with_args(
      "SELECT provsql.get_prob_disk($1) ", 
      1,argtypes,arguments,nulls, false, 0
    ) != SPI_OK_SELECT){
    elog(ERROR, "Something wrong happened while retrieving the probability");
    }
  }
  else {
    if (SPI_execute_with_args(
      "SELECT provsql.get_prob_shmem($1) ", 
      1,argtypes,arguments,nulls, false, 0
    ) != SPI_OK_SELECT){
    elog(ERROR, "Something wrong happened while retrieving the probability");
    }
  }
  entry = SPI_copytuple(SPI_tuptable->vals[0]);
  result = heap_getattr(entry, 1, SPI_tuptable->tupdesc, &isnull);
  SPI_finish();
  if (!isnull)
  {
    PG_RETURN_DATUM(result);
  }
  else {
    PG_RETURN_VOID();
  }

}



PG_FUNCTION_INFO_V1(get_children);
  Datum get_children(PG_FUNCTION_ARGS){
  bool isnull;
  HeapTuple entry;
  Datum result;
  constants_t constants = initialize_constants();
  Datum arguments[1]={datumCopy(PG_GETARG_DATUM(0), false, 16)};
  Oid argtypes[1]={constants.OID_TYPE_PROVENANCE_TOKEN};
  char nulls[1] = {' '};


  SPI_connect();

  if (false && hash_get_num_entries(provsql_hash) >= provsql_max_nb_gates){

    if (SPI_execute_with_args(
      "SELECT provsql.get_children_disk($1) ", 
      1,argtypes,arguments,nulls, false, 0
    ) != SPI_OK_SELECT){
    elog(ERROR, "Something wrong happened while retrieving the probability");
    }
  }
  else {
     if (SPI_execute_with_args(
      "SELECT provsql.get_children_shmem($1) ", 
      1,argtypes,arguments,nulls, false, 0
    ) != SPI_OK_SELECT){
    elog(ERROR, "Something wrong happened while retrieving the probability");
    }   
  }


  entry = SPI_copytuple(SPI_tuptable->vals[0]);
  result = heap_getattr(entry, 1, SPI_tuptable->tupdesc, &isnull);


  SPI_finish();
  if (!isnull)
  {
    ArrayType* array = DatumGetArrayTypePCopy(result);
    PG_RETURN_ARRAYTYPE_P(array);
  }
  else {
    PG_RETURN_VOID();
  }



  }

/*
PG_FUNCTION_INFO_V1(current_memory_usage);
  Datum current_memory_usage(PG_FUNCTION_ARGS){
    provsql_max_nb_gates - hash_get_num_entries 
    PG_RETURN_CSTRING();
  }
*/
