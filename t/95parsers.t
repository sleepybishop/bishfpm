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
done_testing();
