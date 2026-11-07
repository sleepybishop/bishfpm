use strict;
use warnings;
use Test::More;
use t::Util;
my ($err,$out,$status)=run_prog("./t/00util/test_uss");
is $status,0,"USS weighted reference, statistical and engine conformance";
like $out,qr/test_uss passed/,"USS assertions completed";
is $err,"","no USS diagnostics";
done_testing();
