#!/usr/bin/env perl

use strict;

my $path_to_vmtouch = ''; # leave empty if it is in path. otherwise it must end in /
my $frequency = 0.25; # seconds between refreshes

if (!@ARGV) {
  print "Usage: $0 <files to watch>\n";
  exit -1;
}

while(1) {
  my $out = '';
  foreach my $v (@ARGV) {
    $out .= `${path_to_vmtouch}vmtouch -v $v`;
  }
  system("clear");
  print $out;
  select(undef, undef, undef, $frequency);
}
