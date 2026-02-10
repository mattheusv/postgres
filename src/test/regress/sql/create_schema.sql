--
-- CREATE_SCHEMA
--

-- Schema creation with elements.

CREATE ROLE regress_create_schema_role SUPERUSER;

-- Cases where schema creation fails as objects are qualified with a schema
-- that does not match with what's expected.
-- This checks all the object types that include schema qualifications.
CREATE SCHEMA AUTHORIZATION regress_create_schema_role
  CREATE SEQUENCE schema_not_existing.seq;
CREATE SCHEMA AUTHORIZATION regress_create_schema_role
  CREATE TABLE schema_not_existing.tab (id int);
CREATE SCHEMA AUTHORIZATION regress_create_schema_role
  CREATE VIEW schema_not_existing.view AS SELECT 1;
CREATE SCHEMA AUTHORIZATION regress_create_schema_role
  CREATE INDEX ON schema_not_existing.tab (id);
CREATE SCHEMA AUTHORIZATION regress_create_schema_role
  CREATE TRIGGER schema_trig BEFORE INSERT ON schema_not_existing.tab
  EXECUTE FUNCTION schema_trig.no_func();
-- Again, with a role specification and no schema names.
SET ROLE regress_create_schema_role;
CREATE SCHEMA AUTHORIZATION CURRENT_ROLE
  CREATE SEQUENCE schema_not_existing.seq;
CREATE SCHEMA AUTHORIZATION CURRENT_ROLE
  CREATE TABLE schema_not_existing.tab (id int);
CREATE SCHEMA AUTHORIZATION CURRENT_ROLE
  CREATE VIEW schema_not_existing.view AS SELECT 1;
CREATE SCHEMA AUTHORIZATION CURRENT_ROLE
  CREATE INDEX ON schema_not_existing.tab (id);
CREATE SCHEMA AUTHORIZATION CURRENT_ROLE
  CREATE TRIGGER schema_trig BEFORE INSERT ON schema_not_existing.tab
  EXECUTE FUNCTION schema_trig.no_func();
-- Again, with a schema name and a role specification.
CREATE SCHEMA regress_schema_1 AUTHORIZATION CURRENT_ROLE
  CREATE SEQUENCE schema_not_existing.seq;
CREATE SCHEMA regress_schema_1 AUTHORIZATION CURRENT_ROLE
  CREATE TABLE schema_not_existing.tab (id int);
CREATE SCHEMA regress_schema_1 AUTHORIZATION CURRENT_ROLE
  CREATE VIEW schema_not_existing.view AS SELECT 1;
CREATE SCHEMA regress_schema_1 AUTHORIZATION CURRENT_ROLE
  CREATE INDEX ON schema_not_existing.tab (id);
CREATE SCHEMA regress_schema_1 AUTHORIZATION CURRENT_ROLE
  CREATE TRIGGER schema_trig BEFORE INSERT ON schema_not_existing.tab
  EXECUTE FUNCTION schema_trig.no_func();
RESET ROLE;

-- Cases where the schema creation succeeds.
-- The schema created matches the role name.
CREATE SCHEMA AUTHORIZATION regress_create_schema_role
  CREATE TABLE regress_create_schema_role.tab (id int);
\d regress_create_schema_role.tab
DROP SCHEMA regress_create_schema_role CASCADE;
-- Again, with a different role specification and no schema names.
SET ROLE regress_create_schema_role;
CREATE SCHEMA AUTHORIZATION CURRENT_ROLE
  CREATE TABLE regress_create_schema_role.tab (id int);
\d regress_create_schema_role.tab
DROP SCHEMA regress_create_schema_role CASCADE;
-- Again, with a schema name and a role specification.
CREATE SCHEMA regress_schema_1 AUTHORIZATION CURRENT_ROLE
  CREATE TABLE regress_schema_1.tab (id int);
\d regress_schema_1.tab
DROP SCHEMA regress_schema_1 CASCADE;
RESET ROLE;

--
-- CREATE SCHEMA ... LIKE tests
--

-- Create a source schema with various objects
CREATE SCHEMA regress_source_schema;

CREATE TABLE regress_source_schema.t1 (
    id int GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    name text NOT NULL,
    created_at timestamp DEFAULT now()
);

CREATE TABLE regress_source_schema.t2 (
    id int REFERENCES regress_source_schema.t1(id),
    data jsonb
);

CREATE INDEX idx_t1_name ON regress_source_schema.t1(name);
CREATE INDEX idx_t2_data ON regress_source_schema.t2 USING gin(data);

CREATE UNLOGGED TABLE regress_source_schema.t3(a int);

-- Test basic LIKE with TABLE
CREATE SCHEMA regress_copy1 LIKE regress_source_schema INCLUDING TABLE;

-- Verify tables were copied
SELECT table_name FROM information_schema.tables
WHERE table_schema = 'regress_copy1' ORDER BY table_name;

-- Verify table structure (should have columns but not indexes)
\d regress_copy1.t1

-- Verify that relpersistence is the same
SELECT relpersistence, relname from pg_class where relname = 't3' and relnamespace = 'regress_copy1'::regnamespace::oid;;

-- Test LIKE with TABLE and INDEX
CREATE SCHEMA regress_copy2 LIKE regress_source_schema INCLUDING TABLE INCLUDING INDEX;

-- Verify indexes were copied (the name should be the same)
SELECT indexname FROM pg_indexes WHERE schemaname = 'regress_copy2' ORDER BY indexname;

-- Verify table structure (should have columns and indexes)
\d regress_copy2.t1

-- Test EXCLUDING option
CREATE SCHEMA regress_copy3 LIKE regress_source_schema INCLUDING ALL EXCLUDING INDEX;

-- Should have tables but no indexes
SELECT table_name FROM information_schema.tables
WHERE table_schema = 'regress_copy3' ORDER BY table_name;
SELECT indexname FROM pg_indexes WHERE schemaname = 'regress_copy3' ORDER BY indexname;

-- Test IF NOT EXISTS with LIKE
CREATE SCHEMA IF NOT EXISTS regress_copy1 LIKE regress_source_schema INCLUDING TABLE;

-- Test empty source schema
CREATE SCHEMA regress_empty_source;
CREATE SCHEMA regress_copy4 LIKE regress_empty_source INCLUDING ALL;
SELECT table_name FROM information_schema.tables
WHERE table_schema = 'regress_copy4' ORDER BY table_name;

-- Test source schema does not exist
CREATE SCHEMA regress_copy_fail LIKE nonexistent_schema INCLUDING TABLE;

--
-- Test partitioned tables handling
--

-- Create a schema with partitioned table and partitions
CREATE SCHEMA regress_part_source;

CREATE TABLE regress_part_source.sales (
    id int,
    sale_date date,
    amount numeric
) PARTITION BY RANGE (sale_date);

CREATE TABLE regress_part_source.sales2 (
    id SERIAL,
    amount NUMERIC,
    sale_date DATE
) PARTITION BY RANGE (EXTRACT(YEAR FROM sale_date));

CREATE TABLE regress_part_source.sales_2025 PARTITION OF regress_part_source.sales2 FOR VALUES FROM (2024) TO (2025);
CREATE TABLE regress_part_source.sales_2026 PARTITION OF regress_part_source.sales2 FOR VALUES FROM (2025) TO (2026);

CREATE TABLE regress_part_source.sales_2023 PARTITION OF regress_part_source.sales
    FOR VALUES FROM ('2023-01-01') TO ('2024-01-01');

CREATE TABLE regress_part_source.sales_2024 PARTITION OF regress_part_source.sales
    FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');

-- Copy the schema - partitioned table should preserve partition spec
-- and partitions should be attached
CREATE SCHEMA regress_part_copy LIKE regress_part_source INCLUDING ALL;

-- Verify the partitioned table was created with partition spec and that
-- partitions were attached
\d+ regress_part_copy.sales
\d+ regress_part_copy.sales2

-- Test orphan partitions: parent in different schema
-- Create partition in different schema than its parent
CREATE SCHEMA regress_orphan_source;
CREATE TABLE regress_orphan_source.orphan_part PARTITION OF regress_part_source.sales
    FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');

-- Copy the schema with orphan partition - should skip with WARNING
CREATE SCHEMA regress_orphan_copy LIKE regress_orphan_source INCLUDING ALL;

-- The orphan partition should NOT be copied (it was skipped)
\d regress_orphan_copy.*

-- Test list partitioning
CREATE SCHEMA regress_list_part_source;

CREATE TABLE regress_list_part_source.products (
    id int,
    category text,
    name text
) PARTITION BY LIST (category);

CREATE TABLE regress_list_part_source.products_electronics
    PARTITION OF regress_list_part_source.products
    FOR VALUES IN ('electronics', 'computers');

CREATE TABLE regress_list_part_source.products_clothing
    PARTITION OF regress_list_part_source.products
    FOR VALUES IN ('clothing', 'shoes');

CREATE SCHEMA regress_list_part_copy LIKE regress_list_part_source INCLUDING ALL;

-- Verify list partitioned table structure and that partitions were attached correctly
\d+ regress_list_part_copy.products

-- Test hash partitioning
CREATE SCHEMA regress_hash_part_source;

CREATE TABLE regress_hash_part_source.events (
    id int,
    event_type text
) PARTITION BY HASH (id);

CREATE TABLE regress_hash_part_source.events_0
    PARTITION OF regress_hash_part_source.events
    FOR VALUES WITH (MODULUS 2, REMAINDER 0);

CREATE TABLE regress_hash_part_source.events_1
    PARTITION OF regress_hash_part_source.events
    FOR VALUES WITH (MODULUS 2, REMAINDER 1);

CREATE SCHEMA regress_hash_part_copy LIKE regress_hash_part_source INCLUDING ALL;

-- Verify hash partitioned table structure and that partitions were attached correctly
\d+ regress_hash_part_copy.events

-- Clean up partition tests
DROP SCHEMA regress_part_source CASCADE;
DROP SCHEMA regress_part_copy CASCADE;
DROP SCHEMA regress_orphan_source CASCADE;
DROP SCHEMA regress_orphan_copy CASCADE;
DROP SCHEMA regress_list_part_source CASCADE;
DROP SCHEMA regress_list_part_copy CASCADE;
DROP SCHEMA regress_hash_part_source CASCADE;
DROP SCHEMA regress_hash_part_copy CASCADE;

-- Clean up LIKE tests
DROP SCHEMA regress_copy1 CASCADE;
DROP SCHEMA regress_copy2 CASCADE;
DROP SCHEMA regress_copy3 CASCADE;
DROP SCHEMA regress_copy4 CASCADE;
DROP SCHEMA regress_copy_func CASCADE;
DROP SCHEMA regress_empty_source CASCADE;
DROP SCHEMA regress_source_schema CASCADE;

-- Clean up
DROP ROLE regress_create_schema_role;
