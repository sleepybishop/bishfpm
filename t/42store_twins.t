use strict;
use warnings;
use Test::More;
use t::Util;
for my $mode (0, 1) {
  for my $kind (0 .. 4) {
    my ($err, $direct, $status) = run_prog("./t/00util/bench_engines $kind 3000 trace $mode 0");
    is $status, 0, "direct kind=$kind mode=$mode succeeds";
    is $err, "", "direct trace has no diagnostics";
    my ($indirect_err, $indirect, $indirect_status) = run_prog("./t/00util/bench_engines $kind 3000 trace $mode 1");
    is $indirect_status, 0, "indirect kind=$kind mode=$mode succeeds";
    is $indirect_err, "", "indirect trace has no diagnostics";
    is $indirect, $direct, "acceptance, bounds and intermediate callbacks match exactly";
  }
}
done_testing();
