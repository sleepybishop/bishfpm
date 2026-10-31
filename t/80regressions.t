use strict;
use warnings;
use Test::More;
use t::Util;

check_native('test_regressions', qr/test_regressions passed/);
done_testing();
