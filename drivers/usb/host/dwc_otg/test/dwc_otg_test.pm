package dwc_otg_test;

use strict;
use Exporter ();

use vars qw(@ISA @EXPORT
$sysfsdir $paramdir $errors $params
);

@ISA = qw(Exporter);

#
# Globals
#
$sysfsdir = "/sys/devices/lm0";
$paramdir = "/sys/module/dwc_otg";
$errors = 0;

$params = [
	   {
	    NAME => "otg_cap",
	    DEFAULT => 0,
	    ENUM => [],
	    LOW => 0,
	    HIGH => 2
	   },
	   {
	    NAME => "dma_enable",
	    DEFAULT => 0,
	    ENUM => [],
	    LOW => 0,
	    HIGH => 1
	   },
	   {
	    NAME => "dma_burst_size",
	    DEFAULT => 32,
	    ENUM => [1, 4, 8, 16, 32, 64, 128, 256],
	    LOW => 1,
	    HIGH => 256
	   },
	   {
	    NAME => "host_speed",
	    DEFAULT => 0,
	    ENUM => [],
	    LOW => 0,
	    HIGH => 1
	   },
	   {
	    NAME => "host_support_fs_ls_low_power",
	    DEFAULT => 0,
	    ENUM => [],
	    LOW => 0,
	    HIGH => 1
	   },
	   {
	    NAME => "host_ls_low_power_phy_clk",
	    DEFAULT => 0,
	    ENUM => [],
	    LOW => 0,
	    HIGH => 1
	   },
	   {
	    NAME => "dev_speed",
	    DEFAULT => 0,
	    ENUM => [],
	    LOW => 0,
	    HIGH => 1
	   },
	   {
	    NAME => "enable_dynamic_fifo",
	    DEFAULT => 1,
	    ENUM => [],
	    LOW => 0,
	    HIGH => 1
	   },
	   {
	    NAME => "data_fifo_size",
	    DEFAULT => 8192,
	    ENUM => [],
	    LOW => 32,
	    HIGH => 32768
	   },
	   {
	    NAME => "dev_rx_fifo_size",
	    DEFAULT => 1064,
	    ENUM => [],
	    LOW => 16,
	    HIGH => 32768
	   },
	   {
	    NAME => "dev_nperio_tx_fifo_size",
	    DEFAULT => 1024,
	    ENUM => [],
	    LOW => 16,
	    HIGH => 32768
	   },
	   {
	    NAME => "dev_perio_tx_fifo_size_1",
	    DEFAULT => 256,
	    ENUM => [],
	    LOW => 4,
	    HIGH => 768
	   },
	   {
	    NAME => "dev_perio_tx_fifo_size_2",
	    DEFAULT => 256,
	    ENUM => [],
	    LOW => 4,
	    HIGH => 768
	   },
	   {
	    NAME => "dev_perio_tx_fifo_size_3",
	    DEFAULT => 256,
	    ENUM => [],
	    LOW => 4,
	    HIGH => 768
	   },
	   {
	    NAME => "dev_perio_tx_fifo_size_4",
	    DEFAULT => 256,
	    ENUM => [],
	    LOW => 4,
	    HIGH => 768
	   },
	   {
	    NAME => "dev_perio_tx_fifo_size_5",
	    DEFAULT => 256,
	    ENUM => [],
	    LOW => 4,
	    HIGH => 768
	   },
	   {
	    NAME => "dev_perio_tx_fifo_size_6",
	    DEFAULT => 256,
	    ENUM => [],
	    LOW => 4,
	    HIGH => 768
	   },
	   {
	    NAME => "dev_perio_tx_fifo_size_7",
	    DEFAULT => 256,
	    ENUM => [],
	    LOW => 4,
	    HIGH => 768
	   },
	   {
	    NAME => "dev_perio_tx_fifo_size_8",
	    DEFAULT => 256,
	    ENUM => [],
	    LOW => 4,
	    HIGH => 768
	   },
	   {
	    NAME => "dev_perio_tx_fifo_size_9",
	    DEFAULT => 256,
	    ENUM => [],
	    LOW => 4,
	    HIGH => 768
	   },
	   {
	    NAME => "dev_perio_tx_fifo_size_10",
	    DEFAULT => 256,
	    ENUM => [],
	    LOW => 4,
	    HIGH => 768
	   },
	   {
	    NAME => "dev_perio_tx_fifo_size_11",
	    DEFAULT => 256,
	    ENUM => [],
	    LOW => 4,
	    HIGH => 768
	   },
	   {
	    NAME => "dev_perio_tx_fifo_size_12",
	    DEFAULT => 256,
	    ENUM => [],
	    LOW => 4,
	    HIGH => 768
	   },
	   {
	    NAME => "dev_perio_tx_fifo_size_13",
	    DEFAULT => 256,
	    ENUM => [],
	    LOW => 4,
	    HIGH => 768
	   },
	   {
	    NAME => "dev_perio_tx_fifo_size_14",
	    DEFAULT => 256,
	    ENUM => [],
	    LOW => 4,
	    HIGH => 768
	   },
	   {
	    NAME => "dev_perio_tx_fifo_size_15",
	    DEFAULT => 256,
	    ENUM => [],
	    LOW => 4,
	    HIGH => 768
	   },
	   {
	    NAME => "host_rx_fifo_size",
	    DEFAULT => 1024,
	    ENUM => [],
	    LOW => 16,
	    HIGH => 32768
	   },
	   {
	    NAME => "host_nperio_tx_fifo_size",
	    DEFAULT => 1024,
	    ENUM => [],
	    LOW => 16,
	    HIGH => 32768
	   },
	   {
	    NAME => "host_perio_tx_fifo_size",
	    DEFAULT => 1024,
	    ENUM => [],
	    LOW => 16,
	    HIGH => 32768
	   },
	   {
	    NAME => "max_transfer_size",
	    DEFAULT => 65535,
	    ENUM => [],
	    LOW => 2047,
	    HIGH => 65535
	   },
	   {
	    NAME => "max_packet_count",
	    DEFAULT => 511,
	    ENUM => [],
	    LOW => 15,
	    HIGH => 511
	   },
	   {
	    NAME => "host_channels",
	    DEFAULT => 12,
	    ENUM => [],
	    LOW => 1,
	    HIGH => 16
	   },
	   {
	    NAME => "dev_endpoints",
	    DEFAULT => 6,
	    ENUM => [],
	    LOW => 1,
	    HIGH => 15
	   },
	   {
	    NAME => "phy_type",
	    DEFAULT => 1,
	    ENUM => [],
	    LOW => 0,
	    HIGH => 2
	   },
	   {
	    NAME => "phy_utmi_width",
	    DEFAULT => 16,
	    ENUM => [8, 16],
	    LOW => 8,
	    HIGH => 16
	   },
	   {
	    NAME => "phy_ulpi_ddr",
	    DEFAULT => 0,
	    ENUM => [],
	    LOW => 0,
	    HIGH => 1
	   },
	  ];


#
#
sub check_arch {
  $_ = `uname -m`;
  chomp;
  unless (m/armv4tl/) {
    warn "# \n# Can't execute on $_.  Run on integrator platform.\n# \n";
    return 0;
  }
  return 1;
}

#
#
sub load_module {
  my $params = shift;
  print "\nRemoving Module\n";
  system "rmmod dwc_otg";
  print "Loading Module\n";
  if ($params ne "") {
    print "Module Parameters: $params\n";
  }
  if (system("modprobe dwc_otg $params")) {
    warn "Unable to load module\n";
    return 0;
  }
  return 1;
}

#
#
sub test_status {
  my $arg = shift;

  print "\n";

  if (defined $arg) {
    warn "WARNING: $arg\n";
  }

  if ($errors > 0) {
    warn "TEST FAILED with $errors errors\n";
    return 0;
  } else {
    print "TEST PASSED\n";
    return 0 if (defined $arg);
  }
  return 1;
}

#
#
@EXPORT = qw(
$sysfsdir
$paramdir
$params
$errors
check_arch
load_module
test_status
);

1;
