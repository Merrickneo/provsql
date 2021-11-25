\set ECHO none
SET search_path TO provsql_test, provsql;

CREATE TABLE test(id varchar PRIMARY KEY, v integer);

INSERT INTO test(id,v) VALUES
	('hello',4),
	('hi',8);

SELECT add_provenance('test');
SELECT set_prob(provenance(), 0.2) from test where id = 'hello' \g ;
SELECT set_prob(provenance(), 0.3) from test where id = 'hi' \g ;


SELECT dump_data();
SELECT set_prob(provenance(), 0.4) from test where id = 'hello' \g ;


SELECT read_data_dump();

CREATE TABLE result AS SELECT get_prob(provenance()) FROM test;
SELECT remove_provenance('result');
SELECT * FROM result;
DROP TABLE result;
DROP TABLE test;
