use strict;
use warnings;
use Test::More;
use t::Util;

my ($stderr, $stdout) = run_prog("./t/00util/test_itemset");
is $?, 0, "test_itemset executable exits successfully";
like $stdout, qr/test_itemset passed/, "test_itemset output contains pass message";
is $stderr, "", "test_itemset has no stderr diagnostics";

done_testing();
