use strict;
use warnings;
use Test::More;
use t::Util;

my ($stderr, $stdout, $status) =
    run_prog("./examples/stream_tsv data/sample_storefronts.tsv");
is $status, 0, 'five-storefront sample exits successfully';
is $stderr, '', 'five-storefront sample has no rejected records';
like $stdout, qr/Threshold profile: sensitive\n/, 'sample selects its alert thresholds';
like $stdout, qr/Events processed: 18080\b/, 'all hour-long sample events were accepted';

like $stdout,
    qr/\[Group: toy_kr\]\n\s+Pattern:\s+channel=live_stream, device=mobile, item=labubu_doll, ship_country=KR\n/,
    'Labubu burst detects a four-attribute pattern';
like $stdout,
    qr/\[Group: florist_dk\]\n\s+Pattern:\s+campaign=weekend, item=red_tulips, ship_country=DK\n/,
    'second tulip wave detects a three-attribute pattern';
unlike $stdout, qr/\[Group: (?:fashion_us|pantry_it|books_jp)\]/,
    'steady comparison storefronts do not alert';

open my $data, '<', 'data/sample_storefronts.tsv' or die $!;
my %groups;
while (my $line = <$data>) {
    next if $line =~ /^#/;
    my (undef, $group) = split /\t/, $line;
    $groups{$group} = 1;
}
close $data;
is_deeply [sort keys %groups], [qw(books_jp fashion_us florist_dk pantry_it toy_kr)],
    'all five storefront keys are present in the stream';

my @tulip_times = $stdout =~
    /\[\s*([0-9.]+)s\] 🚨 ALERT #\d+ \[Group: florist_dk\]\n\s+Pattern:\s+item=red_tulips, ship_country=DK\n/g;
is scalar @tulip_times, 2, 'red tulips in Denmark alert in two waves';
cmp_ok $tulip_times[0], '>=', 600, 'first tulip wave alerts at 10 minutes';
cmp_ok $tulip_times[0], '<', 605, 'first tulip wave alerts promptly';
cmp_ok $tulip_times[1], '>=', 2400, 'second tulip wave alerts at 40 minutes';
cmp_ok $tulip_times[1], '<', 2405, 'second tulip wave alerts promptly';

my %burst_windows = (
    florist_dk => [[600, 605], [2400, 2405]],
    toy_kr => [[1050, 1055]],
);
my @alerts = $stdout =~ /\[\s*([0-9.]+)s\] 🚨 ALERT #\d+ \[Group: ([^]]+)\]/g;
my $burst_only = @alerts > 0;
while (@alerts) {
    my ($time, $group) = splice @alerts, 0, 2;
    my $windows = $burst_windows{$group} || [];
    $burst_only &&= !!grep { $time >= $_->[0] && $time < $_->[1] } @$windows;
}
ok $burst_only, 'every alert belongs to a scheduled storefront burst';

done_testing();
