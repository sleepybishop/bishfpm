use strict;
use warnings;
use Test::More;
use t::Util;

my ($stderr, $stdout, $status) = run_prog("./t/00util/test_suppression");
is $status, 0, "test_suppression executable exits successfully";
like $stdout, qr/PASS: all suppression, lift, and subsumption tests passed/, "test_suppression output contains pass message";
like $stdout, qr/PASS: partition lift alert quality tests passed/, "partition lift alert quality regressions pass";
like $stdout, qr/PASS: conservative subsumption tests passed/, "conservative subsumption regressions pass";
is $stderr, "", "test_suppression has no stderr diagnostics";

done_testing();
