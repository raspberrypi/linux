#!/usr/bin/perl -w
# 
# Run this program on the integrator.
#
# - Tests module parameter default values.
# - Tests setting of valid module parameter values via modprobe.
# - Tests invalid module parameter values.
# -----------------------------------------------------------------------------
use strict;
use dwc_otg_test;

check_arch() or die;

#
#
sub test {
  my ($param,$expected) = @_;
  my $value = get($param);

  if ($value == $expected) {
    print "$param = $value, okay\n";
  }

  else {
    warn "ERROR: value of $param != $expected, $value\n";
    $errors ++;
  }
}

#
#
sub get {
  my $param = shift;
  my $tmp = `cat $paramdir/$param`;
  chomp $tmp;
  return $tmp;
}

#
#
sub test_main {

  print "\nTesting Module Parameters\n";

  load_module("") or die;

  # Test initial values
  print "\nTesting Default Values\n";
  foreach (@{$params}) {
    test ($_->{NAME}, $_->{DEFAULT});
  }

  # Test low value
  print "\nTesting Low Value\n";
  my $cmd_params = "";
  foreach (@{$params}) {
    $cmd_params = $cmd_params . "$_->{NAME}=$_->{LOW} ";
  }
  load_module($cmd_params) or die;

  foreach (@{$params}) {
    test ($_->{NAME}, $_->{LOW});
  }

  # Test high value
  print "\nTesting High Value\n";
  $cmd_params = "";
  foreach (@{$params}) {
    $cmd_params = $cmd_params . "$_->{NAME}=$_->{HIGH} ";
  }
  load_module($cmd_params) or die;

  foreach (@{$params}) {
    test ($_->{NAME}, $_->{HIGH});
  }

  # Test Enum
  print "\nTesting Enumerated\n";
  foreach (@{$params}) {
    if (defined $_->{ENUM}) {
      my $value;
      foreach $value (@{$_->{ENUM}}) {
	$cmd_params = "$_->{NAME}=$value";
	load_module($cmd_params) or die;
	test ($_->{NAME}, $value);
      }
    }
  }

  # Test Invalid Values
  print "\nTesting Invalid Values\n";
  $cmd_params = "";
  foreach (@{$params}) {
    $cmd_params = $cmd_params . sprintf "$_->{NAME}=%d ", $_->{LOW}-1;
  }
  load_module($cmd_params) or die;

  foreach (@{$params}) {
    test ($_->{NAME}, $_->{DEFAULT});
  }

  $cmd_params = "";
  foreach (@{$params}) {
    $cmd_params = $cmd_params . sprintf "$_->{NAME}=%d ", $_->{HIGH}+1;
  }
  load_module($cmd_params) or die;

  foreach (@{$params}) {
    test ($_->{NAME}, $_->{DEFAULT});
  }

  print "\nTesting Enumerated\n";
  foreach (@{$params}) {
    if (defined $_->{ENUM}) {
      my $value;
      foreach $value (@{$_->{ENUM}}) {
	$value = $value + 1;
	$cmd_params = "$_->{NAME}=$value";
	load_module($cmd_params) or die;
	test ($_->{NAME}, $_->{DEFAULT});
	$value = $value - 2;
	$cmd_params = "$_->{NAME}=$value";
	load_module($cmd_params) or die;
	test ($_->{NAME}, $_->{DEFAULT});
      }
    }
  }

  test_status() or die;
}

test_main();
0;
