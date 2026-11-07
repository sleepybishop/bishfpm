use strict;
use warnings;
use Test::More;
use t::Util;
my ($err,$out,$status)=run_prog("./t/00util/test_heavykeeper");
is $status,0,"HeavyKeeper store and engine conformance";
like $out,qr/test_heavykeeper passed/,"HeavyKeeper assertions completed";
is $err,"","no HeavyKeeper diagnostics";
# Baselines produced from perf@028ae33 using this same replay source with
# -DHK_PERF_REFERENCE, then linked against the original branch sources.
for my $case ([1,1,"96ba6c03c31d4046"],[4,17,"e398bff49317d7ac"]) {
 my ($depth,$width,$digest)=@$case;
 my ($stderr,$stdout,$exit)=run_prog("./t/00util/heavykeeper_trace $depth $width");
 is $exit,0,"legacy replay depth=$depth width=$width";
 like $stdout,qr/digest=$digest/,"estimates, decay, collision RNG and pruning match perf";
 is $stderr,"","trace has no diagnostics";
}
done_testing();
