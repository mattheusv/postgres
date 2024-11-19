# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Test SCRAM authentication pass through the intermediary postgres_fdw to the server

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('node');
my $hostaddr = '127.0.0.1';
my $user = "user01";
my $db1 = "db1";
my $db2 = "db2";
my $fdw_server = "db2_fdw";
my $host = $node->host;
my $port = $node->port;
my $connstr = $node->connstr($db1) . qq' user=$user';

$node->init;
$node->start;

# Test setup

$node->safe_psql('postgres', qq'CREATE USER $user WITH password \'pass\' ');
$node->safe_psql('postgres', qq'CREATE DATABASE $db1');
$node->safe_psql('postgres', qq'CREATE DATABASE $db2');

$node->safe_psql($db2, 'CREATE TABLE t AS SELECT g,g+1 FROM generate_series(1,10) g(g)');
$node->safe_psql($db2, qq'GRANT USAGE ON SCHEMA public to $user');
$node->safe_psql($db2, qq'GRANT SELECT ON t to $user');

$node->safe_psql($db1, 'CREATE EXTENSION IF NOT EXISTS postgres_fdw');
$node->safe_psql($db1, qq'CREATE SERVER $fdw_server FOREIGN DATA WRAPPER postgres_fdw options (
	host \'$host\', port \'$port\', dbname \'$db2\', use_scram_passthrough \'true\') ');
# password not required
$node->safe_psql($db1, qq'CREATE USER MAPPING FOR $user SERVER $fdw_server OPTIONS (user \'$user\');');
$node->safe_psql($db1, qq'GRANT USAGE ON FOREIGN SERVER $fdw_server to $user;');
$node->safe_psql($db1, qq'GRANT ALL ON SCHEMA public to $user');

unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf(
	'pg_hba.conf', qq{
local   all             all                                     scram-sha-256
host    all             all             $hostaddr/32            scram-sha-256
});
$node->restart;

# End of test setup

$ENV{PGPASSWORD} = "pass";

$node->safe_psql($db1, qq'IMPORT FOREIGN SCHEMA public LIMIT TO(t) FROM SERVER $fdw_server INTO public ;',
	connstr=>$connstr);

my $ret = $node->safe_psql($db1, 'SELECT count(1) FROM t',
	connstr=>$connstr);
is($ret, '10', 'SELECT count from fdw server returns 10');


done_testing();
