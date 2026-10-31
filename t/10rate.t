use strict;
use warnings;
use Test::More;
use t::Util;

my ($stderr, $stdout) = run_prog("./t/00util/test_rate");
is $?, 0, "test_rate executable exits successfully";
like $stdout, qr/test_rate passed/, "test_rate output contains pass message";
is $stderr, "", "test_rate has no stderr diagnostics";

done_testing();
