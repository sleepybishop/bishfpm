package t::Util;

use strict;
use warnings;
use File::Temp qw(tempfile);
use Test::More;

use base qw(Exporter);
our @EXPORT = qw(
    run_prog
    check_native
);

sub run_prog {
    my $cmd = shift;
    my ($tempfh, $tempfn) = tempfile(UNLINK => 1);
    my $stderr = `$cmd 2>&1 > $tempfn`;
    my $status = $?;
    my $stdout = do { local $/; <$tempfh> };
    close $tempfh;
    return ($stderr, $stdout, $status);
}

sub check_native {
    my ($program, $marker) = @_;
    my ($err, $out, $status) = run_prog("./t/00util/$program");
    is $status, 0, "$program exits successfully";
    like $out, $marker, "$program completed assertions";
    is $err, '', "$program has no diagnostics";
}

1;
