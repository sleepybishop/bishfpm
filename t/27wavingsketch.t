use strict;
use warnings;
use Test::More;
use t::Util;
my ($err,$out,$status)=run_prog("./t/00util/test_wavingsketch");
is $status,0,"WavingSketch reference, signed counters and engine conformance";
like $out,qr/test_wavingsketch passed/,"WavingSketch assertions completed";
is $err,"","no WavingSketch diagnostics";
done_testing();
