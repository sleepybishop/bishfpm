use strict;
use warnings;
use Test::More;
use t::Util;
my ($err,$out,$status)=run_prog("./t/00util/test_heavykeeper");
is $status,0,"HeavyKeeper store and engine conformance";
like $out,qr/test_heavykeeper passed/,"HeavyKeeper assertions completed";
is $err,"","no HeavyKeeper diagnostics";
# The four-row replay includes matching buckets below .01; their remaining
# counts now accumulate with the next update instead of being discarded.
for my $case ([1,1,"96ba6c03c31d4046"],[4,17,"71b526a0dffd9dc3"]) {
 my ($depth,$width,$digest)=@$case;
 my ($stderr,$stdout,$exit)=run_prog("./t/00util/heavykeeper_trace $depth $width");
 is $exit,0,"deterministic replay depth=$depth width=$width";
 like $stdout,qr/digest=$digest/,"estimates, decay, collision RNG and pruning match baseline";
 is $stderr,"","trace has no diagnostics";
}
done_testing();
