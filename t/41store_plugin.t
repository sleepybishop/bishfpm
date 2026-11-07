use strict;
use warnings;
use Test::More;
use t::Util;
my ($stderr, $stdout, $status) = run_prog("./t/00util/test_store_plugin");
is $status, 0, "store plugin conformance succeeds";
like $stdout, qr/test_store_plugin passed/, "store plugin assertions completed";
is $stderr, "", "store plugin has no diagnostics";
done_testing();
