use strict;
use warnings;
use Test::More;
use t::Util;

my ($stderr, $stdout, $status) = run_prog("./t/00util/test_engine");
is $status, 0, "test_engine executable exits successfully";
like $stdout, qr/detected \d+ burst triggers/, "test_engine detected burst triggers";
like $stdout, qr/country=RU, item=PS5/, "test_engine detected the PS5 foreign card pattern";
is $stderr, "", "test_engine has no stderr diagnostics";

done_testing();
