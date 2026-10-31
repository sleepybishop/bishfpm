package t::Util;

use strict;
use warnings;
use File::Temp qw(tempfile);
use Test::More;

use base qw(Exporter);
our @EXPORT = qw(
    run_prog
);

sub run_prog {
    my $cmd = shift;
    my ($tempfh, $tempfn) = tempfile(UNLINK => 1);
    my $stderr = `$cmd 2>&1 > $tempfn`;
    my $stdout = do { local $/; <$tempfh> };
    close $tempfh;
    return ($stderr, $stdout);
}

1;
