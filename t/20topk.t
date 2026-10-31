use strict;
use warnings;
use Test::More;
use t::Util;

my ($stderr, $stdout) = run_prog("./t/00util/test_topk");
is $?, 0, "test_topk executable exits successfully";
like $stdout, qr/test_topk passed/, "test_topk output contains pass message";
is $stderr, "", "test_topk has no stderr diagnostics";

done_testing();
