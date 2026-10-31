use strict;
use warnings;
use Test::More;
use t::Util;

my ($stderr, $stdout) = run_prog("./examples/fraud_burst");
is $?, 0, "fraud_burst example runs and exits successfully";
like $stdout, qr/\[ALERT\] Group 'store_san_francisco'/, "fraud_burst emits expected alerts";
like $stdout, qr/PlayStation_5/, "fraud_burst detects PlayStation_5 anomaly";
like $stdout, qr/Simulation completed successfully/, "simulation completed banner present";
is $stderr, "", "fraud_burst has no stderr diagnostics";

done_testing();
