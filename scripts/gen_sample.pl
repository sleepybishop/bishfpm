#!/usr/bin/env perl
use strict;
use warnings;

my $out_file = $ARGV[0] || "data/sample_checkout.tsv";
open my $out, ">", $out_file or die "cannot open $out_file: $!";

# parameters
my $t_end = 600.0;
my $burst_start = 420.0;
my $burst_end = 500.0;

# rates in events per second
my $normal_rate = 300.0 / 60.0;     # 5.0 / sec
my $burst_rate  = 1000.0 / 60.0;    # 16.6667 / sec

# attribute dictionaries
my @normal_items = (
    "item=coffee_maker", "item=wireless_mouse", "item=running_shoes",
    "item=bluetooth_headphones", "item=electric_kettle", "item=gaming_keyboard",
    "item=yoga_mat", "item=backpack", "item=smart_watch", "item=desk_lamp"
);

my @normal_countries = ("card_country=US", "card_country=US", "card_country=US", 
                        "card_country=US", "card_country=CA", "card_country=GB");

my @user_agents = ("ua=chrome", "ua=safari", "ua=firefox", "ua=edge", "ua=curl_bot");
my @payments = ("pay=visa", "pay=mastercard", "pay=amex", "pay=paypal", "pay=crypto");
my @shippings = ("ship=standard", "ship=express", "ship=overnight", "ship=in_store_pickup");
my @currencies = ("cur=USD", "cur=EUR", "cur=GBP", "cur=CAD", "cur=JPY");
my @categories = ("cat=electronics", "cat=gaming", "cat=apparel", "cat=home", "cat=grocery");
my @auth_types = ("auth=3ds_secure", "auth=cvv_only", "auth=biometric", "auth=none");
my @devices = ("dev=desktop", "dev=mobile_ios", "dev=mobile_android", "dev=tablet");
my @ip_risks = ("risk=low", "risk=low", "risk=low", "risk=medium", "risk=high", "risk=tor_exit_node");

sub rand_choice {
    my ($arr) = @_;
    return $arr->[int(rand(@$arr))];
}

my $t = 0.0;
my $event_count = 0;
my $burst_count = 0;

while ($t < $t_end) {
    # determine current arrival rate
    my $is_burst = ($t >= $burst_start && $t <= $burst_end);
    my $rate = $is_burst ? $burst_rate : $normal_rate;

    # exponential inter-arrival time (Poisson process)
    my $u = rand();
    $u = 1e-7 if $u == 0;
    my $dt = -log($u) / $rate;
    $t += $dt;
    last if $t > $t_end;

    my $item;
    my $country;

    if ($is_burst) {
        # during burst, 75% of events are the attack signature
        if (rand() < 0.75) {
            $item = "item=playstation_5";
            $country = "card_country=RU";
            $burst_count++;
        } else {
            $item = rand_choice(\@normal_items);
            $country = rand_choice(\@normal_countries);
        }
    } else {
        $item = rand_choice(\@normal_items);
        $country = rand_choice(\@normal_countries);
    }

    # 8 random transaction variables
    my @attrs = (
        $item,
        $country,
        rand_choice(\@user_agents),
        rand_choice(\@payments),
        rand_choice(\@shippings),
        rand_choice(\@currencies),
        rand_choice(\@categories),
        rand_choice(\@auth_types),
        rand_choice(\@devices),
        rand_choice(\@ip_risks)
    );

    # write TSV line: time_sec \t group \t item1 \t item2 ...
    printf $out "%.3f\tcheckout\t%s\n", $t, join("\t", @attrs);
    $event_count++;
}

close $out;

printf "Generated %d events (with %d burst attack events) in %s\n",
    $event_count, $burst_count, $out_file;
