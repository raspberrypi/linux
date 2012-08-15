#!/usr/bin/perl -w
# 
# Run this program on the integrator
# - Tests select sysfs attributes.
# - Todo ... test more attributes, hnp/srp, buspower/bussuspend, etc.
# -----------------------------------------------------------------------------
use strict;
use dwc_otg_test;

check_arch() or die;

#
#
sub test {
  my ($attr,$expected) = @_;
  my $string = get($attr);

  if ($string eq $expected) {
    printf("$attr = $string, okay\n");
  }
  else {
    warn "ERROR: value of $attr != $expected, $string\n";
    $errors ++;
  }
}

#
#
sub set {
  my ($reg, $value) = @_;
  system "echo $value > $sysfsdir/$reg";
}

#
#
sub get {
  my $attr = shift;
  my $string = `cat $sysfsdir/$attr`;
  chomp $string;
  if ($string =~ m/\s\=\s/) {
    my $tmp;
    ($tmp, $string) = split /\s=\s/, $string;
  }
  return $string;
}

#
#
sub test_main {
  print("\nTesting Sysfs Attributes\n");

  load_module("") or die;

  # Test initial values of regoffset/regvalue/guid/gsnpsid
  print("\nTesting Default Values\n");

  test("regoffset", "0xffffffff");
  test("regvalue", "invalid offset");
  test("guid", "0x12345678");	# this will fail if it has been changed
  test("gsnpsid", "0x4f54200a");

  # Test operation of regoffset/regvalue
  print("\nTesting regoffset\n");
  set('regoffset', '5a5a5a5a');
  test("regoffset", "0xffffffff");

  set('regoffset', '0');
  test("regoffset", "0x00000000");

  set('regoffset', '40000');
  test("regoffset", "0x00000000");

  set('regoffset', '3ffff');
  test("regoffset", "0x0003ffff");

  set('regoffset', '1');
  test("regoffset", "0x00000001");

  print("\nTesting regvalue\n");
  set('regoffset', '3c');
  test("regvalue", "0x12345678");
  set('regvalue', '5a5a5a5a');
  test("regvalue", "0x5a5a5a5a");
  set('regvalue','a5a5a5a5');
  test("regvalue", "0xa5a5a5a5");
  set('guid','12345678');

  # Test HNP Capable
  print("\nTesting HNP Capable bit\n");
  set('hnpcapable', '1');
  test("hnpcapable", "0x1");
  set('hnpcapable','0');
  test("hnpcapable", "0x0");

  set('regoffset','0c');

  my $old = get('gusbcfg');
  print("setting hnpcapable\n");
  set('hnpcapable', '1');
  test("hnpcapable", "0x1");
  test('gusbcfg', sprintf "0x%08x", (oct ($old) | (1<<9)));
  test('regvalue', sprintf "0x%08x", (oct ($old) | (1<<9)));

  $old = get('gusbcfg');
  print("clearing hnpcapable\n");
  set('hnpcapable', '0');
  test("hnpcapable", "0x0");
  test ('gusbcfg', sprintf "0x%08x", oct ($old) & (~(1<<9)));
  test ('regvalue', sprintf "0x%08x", oct ($old) & (~(1<<9)));

  # Test SRP Capable
  print("\nTesting SRP Capable bit\n");
  set('srpcapable', '1');
  test("srpcapable", "0x1");
  set('srpcapable','0');
  test("srpcapable", "0x0");

  set('regoffset','0c');

  $old = get('gusbcfg');
  print("setting srpcapable\n");
  set('srpcapable', '1');
  test("srpcapable", "0x1");
  test('gusbcfg', sprintf "0x%08x", (oct ($old) | (1<<8)));
  test('regvalue', sprintf "0x%08x", (oct ($old) | (1<<8)));

  $old = get('gusbcfg');
  print("clearing srpcapable\n");
  set('srpcapable', '0');
  test("srpcapable", "0x0");
  test('gusbcfg', sprintf "0x%08x", oct ($old) & (~(1<<8)));
  test('regvalue', sprintf "0x%08x", oct ($old) & (~(1<<8)));

  # Test GGPIO
  print("\nTesting GGPIO\n");
  set('ggpio','5a5a5a5a');
  test('ggpio','0x5a5a0000');
  set('ggpio','a5a5a5a5');
  test('ggpio','0xa5a50000');
  set('ggpio','11110000');
  test('ggpio','0x11110000');
  set('ggpio','00001111');
  test('ggpio','0x00000000');

  # Test DEVSPEED
  print("\nTesting DEVSPEED\n");
  set('regoffset','800');
  $old = get('regvalue');
  set('devspeed','0');
  test('devspeed','0x0');
  test('regvalue',sprintf("0x%08x", oct($old) & ~(0x3)));
  set('devspeed','1');
  test('devspeed','0x1');
  test('regvalue',sprintf("0x%08x", oct($old) & ~(0x3) | 1));
  set('devspeed','2');
  test('devspeed','0x2');
  test('regvalue',sprintf("0x%08x", oct($old) & ~(0x3) | 2));
  set('devspeed','3');
  test('devspeed','0x3');
  test('regvalue',sprintf("0x%08x", oct($old) & ~(0x3) | 3));
  set('devspeed','4');
  test('devspeed','0x0');
  test('regvalue',sprintf("0x%08x", oct($old) & ~(0x3)));
  set('devspeed','5');
  test('devspeed','0x1');
  test('regvalue',sprintf("0x%08x", oct($old) & ~(0x3) | 1));


  #  mode	Returns the current mode:0 for device mode1 for host mode	Read
  #  hnp	Initiate the Host Negotiation Protocol.  Read returns the status.	Read/Write
  #  srp	Initiate the Session Request Protocol.  Read returns the status.	Read/Write
  #  buspower	Get or Set the Power State of the bus (0 - Off or 1 - On) 	Read/Write
  #  bussuspend	Suspend the USB bus.	Read/Write
  #  busconnected	Get the connection status of the bus 	Read

  #  gotgctl	Get or set the Core Control Status Register.	Read/Write
  ##  gusbcfg	Get or set the Core USB Configuration Register	Read/Write
  #  grxfsiz	Get or set the Receive FIFO Size Register	Read/Write
  #  gnptxfsiz	Get or set the non-periodic Transmit Size Register	Read/Write
  #  gpvndctl	Get or set the PHY Vendor Control Register	Read/Write
  ##  ggpio	Get the value in the lower 16-bits of the General Purpose IO Register or Set the upper 16 bits.	Read/Write
  ##  guid	Get or set the value of the User ID Register	Read/Write
  ##  gsnpsid	Get the value of the Synopsys ID Regester	Read
  ##  devspeed	Get or set the device speed setting in the DCFG register	Read/Write
  #  enumspeed	Gets the device enumeration Speed.	Read
  #  hptxfsiz	Get the value of the Host Periodic Transmit FIFO	Read
  #  hprt0	Get or Set the value in the Host Port Control and Status Register	Read/Write

  test_status("TEST NYI") or die;
}

test_main();
0;
