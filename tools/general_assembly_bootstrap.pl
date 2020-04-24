#!/usr/bin/env perl

use warnings;
use strict;

use Encode qw(decode);
use List::MoreUtils qw(uniq);
# cpan List::MoreUtils
use JSON::MaybeXS;
# cpan JSON::MaybeXS

use Data::Dumper;

sub trim { my $s = shift; $s =~ s/^\s+|\s+$//g; return $s };

my @shortlog = split /\n/, decode('UTF-8', `git shortlog -sne --since="last 36 months"`, Encode::FB_CROAK);

my %assembly = ();

foreach my $line (@shortlog) {
    my ($count, $name, $email) = $line =~ m/^ *(\d+) *(.*?) <(.*?)>/;

    if ($count < 20) {
        next;
    }

    # make sure name is trimmed
    $name = trim $name;

    # assume people with 50 commits have at least 20 source commits
    if ($count < 50) {
        my $true = 0;
        my @commits = split /(^|\n)commit [a-z0-9]{40}(\n|$)/, decode('UTF-8', `git log --name-only --use-mailmap --author="$email" --since="last 36 months"`, Encode::FB_CROAK);
        foreach my $commit (@commits) {
            if ($commit =~ /\n[\w\/]+\.(c|h|S|asm)/) {
                $true++;
            }
        }

        if ($true < 20) {
            next;
        }
    }

    $assembly{$name} = $email;
}

print encode_json(\%assembly);
