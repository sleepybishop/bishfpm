use strict;
use warnings;
use Test::More;
use t::Util;

my ($stderr, $stdout, $status) = run_prog("./examples/stream_tsv data/sample_checkout.tsv");
is $status, 0, "stream_tsv exits successfully";
is $stderr, "", "stream_tsv has no stderr diagnostics";

# verify alert counts and timings
like $stdout, qr/Events processed: \d+/, "events processed summary present";
like $stdout, qr/Alerts triggered: \d+/, "alerts triggered summary present";
like $stdout, qr/Burst arrival window: 420\.\d+s - 500\.\d+s/, "burst arrival window reported";
like $stdout, qr/Time to detection \(TTD\): [0-9\.]+s/, "time to detection reported";

# first alert must occur at start of burst (around 422s)
like $stdout, qr/\[\s*42[0-9]\.\d{2}s\] 🚨 ALERT #1/, "first alert triggers at onset of burst window";

# pattern must identify playstation_5 and foreign card
like $stdout, qr/card_country=RU, item=playstation_5/, "detected core attack pattern";

# verify no alerts before 420.0s
my @lines = split /\n/, $stdout;
my @alert_times = ();
for my $l (@lines) {
    if ($l =~ /\[\s*([0-9\.]+)s\] 🚨 ALERT/) {
        push @alert_times, $1;
    }
}

ok(scalar(@alert_times) > 0, "at least one alert triggered");
my $all_in_window = 1;
for my $at (@alert_times) {
    if ($at < 420.0 || $at > 505.0) {
        $all_in_window = 0;
    }
}
ok($all_in_window, "all alerts triggered within the 420s-500s burst window");

done_testing();
