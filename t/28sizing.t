use strict;
use warnings;
use Test::More;
use t::Util;

my ($err, $out, $status) = run_prog('./t/00util/bench_general --sweep 100 42');
is $status, 0, 'capacity sweep and independent count oracle complete';
is $err, '', 'sweep has no diagnostics';
my @lines = split /\n/, $out;
my @header = split /,/, shift @lines;
is scalar(@lines), 48, 'three workloads, eight capacities, two stores';
my %index;
@index{@header} = (0 .. $#header);
my %distinct;
for my $workload (qw(stable burst churn)) {
    subtest $workload => sub {
        for my $capacity (16, 32, 64, 128, 256, 512, 1024, 2048) {
            my @ss = split /,/, shift(@lines) // '';
            my @fss = split /,/, shift(@lines) // '';
            is $ss[$index{store}], 'ss', "$capacity: SS measured";
            is $fss[$index{store}], 'fss', "$capacity: FSS measured";
            for my $row (\@ss, \@fss) {
                is $row->[$index{workload}], $workload, "$capacity: workload";
                is $row->[$index{ss_capacity}], $capacity, "$capacity: budget reference";
                is $row->[$index{false_confirmations}], 0, "$capacity: no false confirmations";
                ok $row->[$index{events_per_second}] > 0, "$capacity: throughput measured";
                ok $row->[$index{store_bytes}] <= $row->[$index{store_budget}],
                    "$capacity: store fits budget";
                is $row->[$index{resident_updates}] + $row->[$index{admissions}]
                    + $row->[$index{filtered_updates}] + $row->[$index{rejected_updates}]
                    + $row->[$index{zero_weight_queries}],
                    $row->[$index{candidate_updates}], "$capacity: pressure accounts for all updates";
                is $row->[$index{rejected_updates}], 0, "$capacity: no store overflow";
                ok $row->[$index{resident_patterns}] <= $row->[$index{resident_capacity}],
                    "$capacity: pressure occupancy fits capacity";
                ok $row->[$index{current_bytes}] <= $row->[$index{peak_bytes}]
                    && $row->[$index{peak_bytes}] <= 8 * 1024 * 1024,
                    "$capacity: full engine memory measured within limit";
                if ($workload eq 'stable') {
                    is $row->[$index{burst_first_eligible_event}], -1,
                        "$capacity: stable stream has no eligible burst";
                }
                if ($capacity == 2048) {
                    cmp_ok $row->[$index{alert_recall}], '>=', .99,
                        'ample capacity confirms eligible opportunities';
                }
            }
            for my $field (qw(store_budget eligible_opportunities distinct_patterns candidate_updates)) {
                is $ss[$index{$field}], $fss[$index{$field}], "$capacity: matching $field";
            }
            ok $fss[$index{peak_bytes}] <= $ss[$index{peak_bytes}],
                "$capacity: FSS peak fits SS comparison footprint";
            if ($capacity == 16) {
                $distinct{$workload} = $ss[$index{distinct_patterns}];
            } else {
                is $ss[$index{distinct_patterns}], $distinct{$workload},
                    "$capacity: stream independent of capacity";
            }
        }
    };
}
ok $distinct{stable} < $distinct{burst} && $distinct{burst} < $distinct{churn},
    'workloads exercise different candidate pressures';

# Preserve the old invocation, and reject malformed sizing requests before
# allocating engines or producing a partial CSV.
for my $args ('100 42 128 stable', '100 42 128 burst', '100 42 128 churn') {
    my ($err, $out, $status) = run_prog("./t/00util/bench_general $args");
    is $status, 0, "single capacity: $args";
    is $err, '', 'single capacity has no diagnostics';
    is scalar(split /\n/, $out), 3, 'single capacity emits one comparison';
}
for my $args ('abc', '-1', '100junk', '100 42 15', '100 42 4097',
              '100 42 64 missing', '--sweep 100 42 extra', '100 4294967296',
              '--sweep 99', '100 42 64 stable extra') {
    my ($err, $out, $status) = run_prog("./t/00util/bench_general $args");
    isnt $status, 0, "invalid arguments rejected: $args";
    like $err, qr/^usage:/, 'usage diagnostic';
    is $out, '', 'invalid arguments emit no CSV';
}
done_testing();
