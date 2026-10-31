use strict;
use warnings;
use Test::More;
use File::Temp qw(tempfile);
use t::Util;
sub run_input {
    my ($program,$input)=@_;
    my ($fh,$path)=tempfile('/tmp/bishfpm-parser-XXXXXX',UNLINK=>1);
    binmode $fh;
    print {$fh} $input;
    close $fh;
    my ($err,$out,$status)=run_prog("./examples/$program $path");
    return ($status,$err,$out);
}
for my $record ("1\tg\tx\ty", "1\tg\tx\ty\r\n") {
    my ($status,$err,$out)=run_input('stream_tsv',$record);
    is $status,0,'TSV final/CRLF accepted'; is $err,'','no rejection';
    like $out,qr/Events processed: 1/,'one committed event';
}
for my $record ("nan\tg\tx\n","1junk\tg\tx\n","-1\tg\tx\n","1\t\tx\n",
                "1\tg\t\n","1\tg\tx\x1fy\n","1\tg\t".('x'x256)."\n",
                "1\tg\tx\0y\n",'x'x5000,"1\tg\t".join("\t",(1..33))."\n") {
    my ($status,$err,$out)=run_input('stream_tsv',$record."\n2\tg\tok\n");
    isnt $status,0,'malformed TSV makes exit fail';
    like $err,qr/Rejected records: 1/,'TSV rejection counted once';
    like $out,qr/Events processed: 1/,'valid record following bad input processed';
}

# A rejected future timestamp must not expire the accepted history used by
# subsequent valid records, or appear in the reported burst arrival window.
my $history = ("10\tg\tc\n" x 100) . ("10\tg\ta\tb\n" x 40);
my $burst = "11\tg\ta\tb\n" x 100;
my ($baseline_status,$baseline_err,$baseline_out) =
    run_input('stream_tsv',$history.$burst);
is $baseline_status,0,'valid reference stream succeeds';
is $baseline_err,'','valid reference stream has no diagnostics';
like $baseline_out,qr/Alerts triggered: 1\b/,'reference history produces an alert';
for my $invalid_item ("exploit=x\x1fy", 'exploit='.('x'x256)) {
    my ($status,$err,$out) =
        run_input('stream_tsv',$history."1000\tg\t$invalid_item\n".$burst);
    isnt $status,0,'rejected future record makes exit fail';
    like $err,qr/Rejected records: 1; I\/O error: 0/,'future record rejected once';
    like $out,qr/Events processed: 240\b/,'accepted records are unchanged';
    like $out,qr/Alerts triggered: 1\b/,'rejected timestamp preserves the alert';
    unlike $out,qr/Stale items pruned:/,'rejected timestamp does not prune history';
    unlike $out,qr/Burst arrival window:/,'rejected burst marker is not reported';
}

my $other_groups = join '', map { "10\tg$_\tc\n" } 1..31;
my ($capacity_status,$capacity_err,$capacity_out) = run_input('stream_tsv',
    $history.$other_groups."1000\tnew-group\texploit=x\n".$burst);
isnt $capacity_status,0,'future event at group capacity is rejected';
like $capacity_err,qr/Rejected records: 1; I\/O error: 0/,'capacity rejection counted';
like $capacity_out,qr/Events processed: 271\b/,'capacity rejection preserves accepted records';
like $capacity_out,qr/Alerts triggered: 1\b/,'capacity rejection preserves the alert';
unlike $capacity_out,qr/Stale items pruned:/,'capacity rejection does not prune history';
unlike $capacity_out,qr/Burst arrival window:/,'capacity rejection does not report a burst';

my ($schedule_status,$schedule_err,$schedule_out) = run_input('stream_tsv',
    "5\tg\tx\n1000\tg\tx\x1fy\n30\tg\ty\n");
isnt $schedule_status,0,'schedule test reports the rejected record';
like $schedule_err,qr/Rejected records: 1; I\/O error: 0/,'schedule rejection counted';
like $schedule_out,qr/Events processed: 2\b/,'both valid schedule records accepted';
like $schedule_out,qr/Stale items pruned: 1\b/,
    'next accepted timestamp still runs scheduled maintenance';
done_testing();
