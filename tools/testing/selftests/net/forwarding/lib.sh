#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

##############################################################################
# Topology description. p1 looped back to p2, p3 to p4 and so on.

declare -A NETIFS=(
    [p1]=veth0
    [p2]=veth1
    [p3]=veth2
    [p4]=veth3
    [p5]=veth4
    [p6]=veth5
    [p7]=veth6
    [p8]=veth7
    [p9]=veth8
    [p10]=veth9
)

# Port that does not have a cable connected.
: "${NETIF_NO_CABLE:=eth8}"

##############################################################################
# Defines

# Networking utilities.
: "${PING:=ping}"
: "${PING6:=ping6}"	# Some distros just use ping.
: "${ARPING:=arping}"
: "${TROUTE6:=traceroute6}"

# Packet generator.
: "${MZ:=mausezahn}"	# Some distributions use 'mz'.
: "${MZ_DELAY:=0}"

# Host configuration tools.
: "${TEAMD:=teamd}"
: "${MCD:=smcrouted}"
: "${MC_CLI:=smcroutectl}"
: "${MCD_TABLE_NAME:=selftests}"

# Constants for netdevice bring-up:
# Default time in seconds to wait for an interface to come up before giving up
# and bailing out. Used during initial setup.
: "${INTERFACE_TIMEOUT:=600}"
# Like INTERFACE_TIMEOUT, but default for ad-hoc waiting in testing scripts.
: "${WAIT_TIMEOUT:=20}"
# Time to wait after interfaces participating in the test are all UP.
: "${WAIT_TIME:=5}"

# Whether to pause on, respectively, after a failure and before cleanup.
: "${PAUSE_ON_CLEANUP:=no}"

# Whether to create virtual interfaces, and what netdevice type they should be.
: "${NETIF_CREATE:=yes}"
: "${NETIF_TYPE:=veth}"

# Constants for ping tests:
# How many packets should be sent.
: "${PING_COUNT:=10}"
# Timeout (in seconds) before ping exits regardless of how many packets have
# been sent or received
: "${PING_TIMEOUT:=5}"

# Minimum ageing_time (in centiseconds) supported by hardware
: "${LOW_AGEING_TIME:=1000}"

# Whether to check for availability of certain tools.
: "${REQUIRE_JQ:=yes}"
: "${REQUIRE_MZ:=yes}"
: "${REQUIRE_MTOOLS:=no}"
: "${REQUIRE_TEAMD:=no}"

# Whether to override MAC addresses on interfaces participating in the test.
: "${STABLE_MAC_ADDRS:=no}"

# Flags for tcpdump
: "${TCPDUMP_EXTRA_FLAGS:=}"

# Flags for TC filters.
: "${TC_FLAG:=skip_hw}"

# Whether the machine is "slow" -- i.e. might be incapable of running tests
# involving heavy traffic. This might be the case on a debug kernel, a VM, or
# e.g. a low-power board.
: "${KSFT_MACHINE_SLOW:=no}"

##############################################################################
# Find netifs by test-specified driver name

driver_name_get()
{
	local dev=$1; shift
	local driver_path="/sys/class/net/$dev/device/driver"

	if [[ -L $driver_path ]]; then
		basename `realpath $driver_path`
	fi
}

netif_find_driver()
{
	local ifnames=`ip -j link show | jq -r ".[].ifname"`
	local count=0

	for ifname in $ifnames
	do
		local driver_name=`driver_name_get $ifname`
		if [[ ! -z $driver_name && $driver_name == $NETIF_FIND_DRIVER ]]; then
			count=$((count + 1))
			NETIFS[p$count]="$ifname"
		fi
	done
}

# Whether to find netdevice according to the driver speficied by the importer
: "${NETIF_FIND_DRIVER:=}"

if [[ $NETIF_FIND_DRIVER ]]; then
	unset NETIFS
	declare -A NETIFS
	netif_find_driver
fi

net_forwarding_dir=$(dirname "$(readlink -e "${BASH_SOURCE[0]}")")

if [[ -f $net_forwarding_dir/forwarding.config ]]; then
	source "$net_forwarding_dir/forwarding.config"
fi

source "$net_forwarding_dir/../lib.sh"

##############################################################################
# Sanity checks

check_tc_version()
{
	tc -j &> /dev/null
	if [[ $? -ne 0 ]]; then
		echo "SKIP: iproute2 too old; tc is missing JSON support"
		exit $ksft_skip
	fi
}

check_tc_erspan_support()
{
	local dev=$1; shift

	tc filter add dev $dev ingress pref 1 handle 1 flower \
		erspan_opts 1:0:0:0 &> /dev/null
	if [[ $? -ne 0 ]]; then
		echo "SKIP: iproute2 too old; tc is missing erspan support"
		return $ksft_skip
	fi
	tc filter del dev $dev ingress pref 1 handle 1 flower \
		erspan_opts 1:0:0:0 &> /dev/null
}

# Old versions of tc don't understand "mpls_uc"
check_tc_mpls_support()
{
	local dev=$1; shift

	tc filter add dev $dev ingress protocol mpls_uc pref 1 handle 1 \
		matchall action pipe &> /dev/null
	if [[ $? -ne 0 ]]; then
		echo "SKIP: iproute2 too old; tc is missing MPLS support"
		return $ksft_skip
	fi
	tc filter del dev $dev ingress protocol mpls_uc pref 1 handle 1 \
		matchall
}

# Old versions of tc produce invalid json output for mpls lse statistics
check_tc_mpls_lse_stats()
{
	local dev=$1; shift
	local ret;

	tc filter add dev $dev ingress protocol mpls_uc pref 1 handle 1 \
		flower mpls lse depth 2                                 \
		action continue &> /dev/null

	if [[ $? -ne 0 ]]; then
		echo "SKIP: iproute2 too old; tc-flower is missing extended MPLS support"
		return $ksft_skip
	fi

	tc -j filter show dev $dev ingress protocol mpls_uc | jq . &> /dev/null
	ret=$?
	tc filter del dev $dev ingress protocol mpls_uc pref 1 handle 1 \
		flower

	if [[ $ret -ne 0 ]]; then
		echo "SKIP: iproute2 too old; tc-flower produces invalid json output for extended MPLS filters"
		return $ksft_skip
	fi
}

check_tc_shblock_support()
{
	tc filter help 2>&1 | grep block &> /dev/null
	if [[ $? -ne 0 ]]; then
		echo "SKIP: iproute2 too old; tc is missing shared block support"
		exit $ksft_skip
	fi
}

check_tc_chain_support()
{
	tc help 2>&1|grep chain &> /dev/null
	if [[ $? -ne 0 ]]; then
		echo "SKIP: iproute2 too old; tc is missing chain support"
		exit $ksft_skip
	fi
}

check_tc_action_hw_stats_support()
{
	tc actions help 2>&1 | grep -q hw_stats
	if [[ $? -ne 0 ]]; then
		echo "SKIP: iproute2 too old; tc is missing action hw_stats support"
		exit $ksft_skip
	fi
}

check_tc_fp_support()
{
	tc qdisc add dev lo mqprio help 2>&1 | grep -q "fp "
	if [[ $? -ne 0 ]]; then
		echo "SKIP: iproute2 too old; tc is missing frame preemption support"
		exit $ksft_skip
	fi
}

check_ethtool_lanes_support()
{
	ethtool --help 2>&1| grep lanes &> /dev/null
	if [[ $? -ne 0 ]]; then
		echo "SKIP: ethtool too old; it is missing lanes support"
		exit $ksft_skip
	fi
}

check_ethtool_mm_support()
{
	ethtool --help 2>&1| grep -- '--show-mm' &> /dev/null
	if [[ $? -ne 0 ]]; then
		echo "SKIP: ethtool too old; it is missing MAC Merge layer support"
		exit $ksft_skip
	fi
}

check_ethtool_counter_group_support()
{
	ethtool --help 2>&1| grep -- '--all-groups' &> /dev/null
	if [[ $? -ne 0 ]]; then
		echo "SKIP: ethtool too old; it is missing standard counter group support"
		exit $ksft_skip
	fi
}

check_ethtool_pmac_std_stats_support()
{
	local dev=$1; shift
	local grp=$1; shift

	[ 0 -ne $(ethtool --json -S $dev --all-groups --src pmac 2>/dev/null \
		| jq ".[].\"$grp\" | length") ]
}

check_locked_port_support()
{
	if ! bridge -d link show | grep -q " locked"; then
		echo "SKIP: iproute2 too old; Locked port feature not supported."
		return $ksft_skip
	fi
}

check_port_mab_support()
{
	if ! bridge -d link show | grep -q "mab"; then
		echo "SKIP: iproute2 too old; MacAuth feature not supported."
		return $ksft_skip
	fi
}

if [[ "$(id -u)" -ne 0 ]]; then
	echo "SKIP: need root privileges"
	exit $ksft_skip
fi

check_driver()
{
	local dev=$1; shift
	local expected=$1; shift
	local driver_name=`driver_name_get $dev`

	if [[ $driver_name != $expected ]]; then
		echo "SKIP: expected driver $expected for $dev, got $driver_name instead"
		exit $ksft_skip
	fi
}

if [[ "$CHECK_TC" = "yes" ]]; then
	check_tc_version
fi

# IPv6 support was added in v3.0
check_mtools_version()
{
	local version="$(msend -v)"
	local major

	version=${version##msend version }
	major=$(echo $version | cut -d. -f1)

	if [ $major -lt 3 ]; then
		echo "SKIP: expected mtools version 3.0, got $version"
		exit $ksft_skip
	fi
}

if [[ "$REQUIRE_JQ" = "yes" ]]; then
	require_command jq
fi
if [[ "$REQUIRE_MZ" = "yes" ]]; then
	require_command $MZ
fi
if [[ "$REQUIRE_TEAMD" = "yes" ]]; then
	require_command $TEAMD
fi
if [[ "$REQUIRE_MTOOLS" = "yes" ]]; then
	# https://github.com/troglobit/mtools
	require_command msend
	require_command mreceive
	check_mtools_version
fi

##############################################################################
# Command line options handling

count=0

while [[ $# -gt 0 ]]; do
	if [[ "$count" -eq "0" ]]; then
		unset NETIFS
		declare -A NETIFS
	fi
	count=$((count + 1))
	NETIFS[p$count]="$1"
	shift
done

##############################################################################
# Network interfaces configuration

if [[ ! -v NUM_NETIFS ]]; then
	echo "SKIP: importer does not define \"NUM_NETIFS\""
	exit $ksft_skip
fi

if (( NUM_NETIFS > ${#NETIFS[@]} )); then
	echo "SKIP: Importer requires $NUM_NETIFS NETIFS, but only ${#NETIFS[@]} are defined (${NETIFS[@]})"
	exit $ksft_skip
fi

for i in $(seq ${#NETIFS[@]}); do
	if [[ ! ${NETIFS[p$i]} ]]; then
		echo "SKIP: NETIFS[p$i] not given"
		exit $ksft_skip
	fi
done

create_netif_veth()
{
	local i

	for ((i = 1; i <= NUM_NETIFS; ++i)); do
		local j=$((i+1))

		if [ -z ${NETIFS[p$i]} ]; then
			echo "SKIP: Cannot create interface. Name not specified"
			exit $ksft_skip
		fi

		ip link show dev ${NETIFS[p$i]} &> /dev/null
		if [[ $? -ne 0 ]]; then
			ip link add ${NETIFS[p$i]} type veth \
				peer name ${NETIFS[p$j]}
			if [[ $? -ne 0 ]]; then
				echo "Failed to create netif"
				exit 1
			fi
		fi
		i=$j
	done
}

create_netif()
{
	case "$NETIF_TYPE" in
	veth) create_netif_veth
	      ;;
	*) echo "Can not create interfaces of type \'$NETIF_TYPE\'"
	   exit 1
	   ;;
	esac
}

declare -A MAC_ADDR_ORIG
mac_addr_prepare()
{
	local new_addr=
	local dev=

	for ((i = 1; i <= NUM_NETIFS; ++i)); do
		dev=${NETIFS[p$i]}
		new_addr=$(printf "00:01:02:03:04:%02x" $i)

		MAC_ADDR_ORIG["$dev"]=$(ip -j link show dev $dev | jq -e '.[].address')
		# Strip quotes
		MAC_ADDR_ORIG["$dev"]=${MAC_ADDR_ORIG["$dev"]//\"/}
		ip link set dev $dev address $new_addr
	done
}

mac_addr_restore()
{
	local dev=

	for ((i = 1; i <= NUM_NETIFS; ++i)); do
		dev=${NETIFS[p$i]}
		ip link set dev $dev address ${MAC_ADDR_ORIG["$dev"]}
	done
}

if [[ "$NETIF_CREATE" = "yes" ]]; then
	create_netif
fi

if [[ "$STABLE_MAC_ADDRS" = "yes" ]]; then
	mac_addr_prepare
fi

for ((i = 1; i <= NUM_NETIFS; ++i)); do
	ip link show dev ${NETIFS[p$i]} &> /dev/null
	if [[ $? -ne 0 ]]; then
		echo "SKIP: could not find all required interfaces"
		exit $ksft_skip
	fi
done

##############################################################################
# Helpers

not()
{
	"$@"
	[[ $? != 0 ]]
}

get_max()
{
	local arr=("$@")

	max=${arr[0]}
	for cur in ${arr[@]}; do
		if [[ $cur -gt $max ]]; then
			max=$cur
		fi
	done

	echo $max
}

grep_bridge_fdb()
{
	local addr=$1; shift
	local word
	local flag

	if [ "$1" == "self" ] || [ "$1" == "master" ]; then
		word=$1; shift
		if [ "$1" == "-v" ]; then
			flag=$1; shift
		fi
	fi

	$@ | grep $addr | grep $flag "$word"
}

wait_for_port_up()
{
	"$@" | grep -q "Link detected: yes"
}

wait_for_offload()
{
	"$@" | grep -q offload
}

wait_for_trap()
{
	"$@" | grep -q trap
}

setup_wait_dev()
{
	local dev=$1; shift
	local wait_time=${1:-$WAIT_TIME}; shift

	setup_wait_dev_with_timeout "$dev" $INTERFACE_TIMEOUT $wait_time

	if (($?)); then
		check_err 1
		log_test setup_wait_dev ": Interface $dev does not come up."
		exit 1
	fi
}

setup_wait_dev_with_timeout()
{
	local dev=$1; shift
	local max_iterations=${1:-$WAIT_TIMEOUT}; shift
	local wait_time=${1:-$WAIT_TIME}; shift
	local i

	for ((i = 1; i <= $max_iterations; ++i)); do
		ip link show dev $dev up \
			| grep 'state UP' &> /dev/null
		if [[ $? -ne 0 ]]; then
			sleep 1
		else
			sleep $wait_time
			return 0
		fi
	done

	return 1
}

setup_wait_n()
{
	local num_netifs=$1; shift
	local i

	for ((i = 1; i <= num_netifs; ++i)); do
		setup_wait_dev ${NETIFS[p$i]} 0
	done

	# Make sure links are ready.
	sleep $WAIT_TIME
}

setup_wait()
{
	setup_wait_n "$NUM_NETIFS"
}

wait_for_dev()
{
        local dev=$1; shift
        local timeout=${1:-$WAIT_TIMEOUT}; shift

        slowwait $timeout ip link show dev $dev &> /dev/null
        if (( $? )); then
                check_err 1
                log_test wait_for_dev "Interface $dev did not appear."
                exit $EXIT_STATUS
        fi
}

cmd_jq()
{
	local cmd=$1
	local jq_exp=$2
	local jq_opts=$3
	local ret
	local output

	output="$($cmd)"
	# it the command fails, return error right away
	ret=$?
	if [[ $ret -ne 0 ]]; then
		return $ret
	fi
	output=$(echo $output | jq -r $jq_opts "$jq_exp")
	ret=$?
	if [[ $ret -ne 0 ]]; then
		return $ret
	fi
	echo $output
	# return success only in case of non-empty output
	[ ! -z "$output" ]
}

pre_cleanup()
{
	if [ "${PAUSE_ON_CLEANUP}" = "yes" ]; then
		echo "Pausing before cleanup, hit any key to continue"
		read
	fi

	if [[ "$STABLE_MAC_ADDRS" = "yes" ]]; then
		mac_addr_restore
	fi
}

vrf_prepare()
{
	ip -4 rule add pref 32765 table local
	ip -4 rule del pref 0
	ip -6 rule add pref 32765 table local
	ip -6 rule del pref 0
}

vrf_cleanup()
{
	ip -6 rule add pref 0 table local
	ip -6 rule del pref 32765
	ip -4 rule add pref 0 table local
	ip -4 rule del pref 32765
}

__last_tb_id=0
declare -A __TB_IDS

__vrf_td_id_assign()
{
	local vrf_name=$1

	__last_tb_id=$((__last_tb_id + 1))
	__TB_IDS[$vrf_name]=$__last_tb_id
	return $__last_tb_id
}

__vrf_td_id_lookup()
{
	local vrf_name=$1

	return ${__TB_IDS[$vrf_name]}
}

vrf_create()
{
	local vrf_name=$1
	local tb_id

	__vrf_td_id_assign $vrf_name
	tb_id=$?

	ip link add dev $vrf_name type vrf table $tb_id
	ip -4 route add table $tb_id unreachable default metric 4278198272
	ip -6 route add table $tb_id unreachable default metric 4278198272
}

vrf_destroy()
{
	local vrf_name=$1
	local tb_id

	__vrf_td_id_lookup $vrf_name
	tb_id=$?

	ip -6 route del table $tb_id unreachable default metric 4278198272
	ip -4 route del table $tb_id unreachable default metric 4278198272
	ip link del dev $vrf_name
}

__addr_add_del()
{
	local if_name=$1
	local add_del=$2
	local array

	shift
	shift
	array=("${@}")

	for addrstr in "${array[@]}"; do
		ip address $add_del $addrstr dev $if_name
	done
}

__simple_if_init()
{
	local if_name=$1; shift
	local vrf_name=$1; shift
	local addrs=("${@}")

	ip link set dev $if_name master $vrf_name
	ip link set dev $if_name up

	__addr_add_del $if_name add "${addrs[@]}"
}

__simple_if_fini()
{
	local if_name=$1; shift
	local addrs=("${@}")

	__addr_add_del $if_name del "${addrs[@]}"

	ip link set dev $if_name down
	ip link set dev $if_name nomaster
}

simple_if_init()
{
	local if_name=$1
	local vrf_name
	local array

	shift
	vrf_name=v$if_name
	array=("${@}")

	vrf_create $vrf_name
	ip link set dev $vrf_name up
	__simple_if_init $if_name $vrf_name "${array[@]}"
}

simple_if_fini()
{
	local if_name=$1
	local vrf_name
	local array

	shift
	vrf_name=v$if_name
	array=("${@}")

	__simple_if_fini $if_name "${array[@]}"
	vrf_destroy $vrf_name
}

tunnel_create()
{
	local name=$1; shift
	local type=$1; shift
	local local=$1; shift
	local remote=$1; shift

	ip link add name $name type $type \
	   local $local remote $remote "$@"
	ip link set dev $name up
}

tunnel_destroy()
{
	local name=$1; shift

	ip link del dev $name
}

vlan_create()
{
	local if_name=$1; shift
	local vid=$1; shift
	local vrf=$1; shift
	local ips=("${@}")
	local name=$if_name.$vid

	ip link add name $name link $if_name type vlan id $vid
	if [ "$vrf" != "" ]; then
		ip link set dev $name master $vrf
	fi
	ip link set dev $name up
	__addr_add_del $name add "${ips[@]}"
}

vlan_destroy()
{
	local if_name=$1; shift
	local vid=$1; shift
	local name=$if_name.$vid

	ip link del dev $name
}

team_create()
{
	local if_name=$1; shift
	local mode=$1; shift

	require_command $TEAMD
	$TEAMD -t $if_name -d -c '{"runner": {"name": "'$mode'"}}'
	for slave in "$@"; do
		ip link set dev $slave down
		ip link set dev $slave master $if_name
		ip link set dev $slave up
	done
	ip link set dev $if_name up
}

team_destroy()
{
	local if_name=$1; shift

	$TEAMD -t $if_name -k
}

master_name_get()
{
	local if_name=$1

	ip -j link show dev $if_name | jq -r '.[]["master"]'
}

link_stats_get()
{
	local if_name=$1; shift
	local dir=$1; shift
	local stat=$1; shift

	ip -j -s link show dev $if_name \
		| jq '.[]["stats64"]["'$dir'"]["'$stat'"]'
}

link_stats_tx_packets_get()
{
	link_stats_get $1 tx packets
}

link_stats_rx_errors_get()
{
	link_stats_get $1 rx errors
}

ethtool_stats_get()
{
	local dev=$1; shift
	local stat=$1; shift

	ethtool -S $dev | grep "^ *$stat:" | head -n 1 | cut -d: -f2
}

ethtool_std_stats_get()
{
	local dev=$1; shift
	local grp=$1; shift
	local name=$1; shift
	local src=$1; shift

	ethtool --json -S $dev --groups $grp -- --src $src | \
		jq '.[]."'"$grp"'"."'$name'"'
}

qdisc_stats_get()
{
	local dev=$1; shift
	local handle=$1; shift
	local selector=$1; shift

	tc -j -s qdisc show dev "$dev" \
	    | jq '.[] | select(.handle == "'"$handle"'") | '"$selector"
}

qdisc_parent_stats_get()
{
	local dev=$1; shift
	local parent=$1; shift
	local selector=$1; shift

	tc -j -s qdisc show dev "$dev" invisible \
	    | jq '.[] | select(.parent == "'"$parent"'") | '"$selector"
}

ipv6_stats_get()
{
	local dev=$1; shift
	local stat=$1; shift

	cat /proc/net/dev_snmp6/$dev | grep "^$stat" | cut -f2
}

hw_stats_get()
{
	local suite=$1; shift
	local if_name=$1; shift
	local dir=$1; shift
	local stat=$1; shift

	ip -j stats show dev $if_name group offload subgroup $suite |
		jq ".[0].stats64.$dir.$stat"
}

__nh_stats_get()
{
	local key=$1; shift
	local group_id=$1; shift
	local member_id=$1; shift

	ip -j -s -s nexthop show id $group_id |
	    jq --argjson member_id "$member_id" --arg key "$key" \
	       '.[].group_stats[] | select(.id == $member_id) | .[$key]'
}

nh_stats_get()
{
	local group_id=$1; shift
	local member_id=$1; shift

	__nh_stats_get packets "$group_id" "$member_id"
}

nh_stats_get_hw()
{
	local group_id=$1; shift
	local member_id=$1; shift

	__nh_stats_get packets_hw "$group_id" "$member_id"
}

humanize()
{
	local speed=$1; shift

	for unit in bps Kbps Mbps Gbps; do
		if (($(echo "$speed < 1024" | bc))); then
			break
		fi

		speed=$(echo "scale=1; $speed / 1024" | bc)
	done

	echo "$speed${unit}"
}

rate()
{
	local t0=$1; shift
	local t1=$1; shift
	local interval=$1; shift

	echo $((8 * (t1 - t0) / interval))
}

packets_rate()
{
	local t0=$1; shift
	local t1=$1; shift
	local interval=$1; shift

	echo $(((t1 - t0) / interval))
}

ether_addr_to_u64()
{
	local addr="$1"
	local order="$((1 << 40))"
	local val=0
	local byte

	addr="${addr//:/ }"

	for byte in $addr; do
		byte="0x$byte"
		val=$((val + order * byte))
		order=$((order >> 8))
	done

	printf "0x%x" $val
}

u64_to_ether_addr()
{
	local val=$1
	local byte
	local i

	for ((i = 40; i >= 0; i -= 8)); do
		byte=$(((val & (0xff << i)) >> i))
		printf "%02x" $byte
		if [ $i -ne 0 ]; then
			printf ":"
		fi
	done
}

ipv6_lladdr_get()
{
	local if_name=$1

	ip -j addr show dev $if_name | \
		jq -r '.[]["addr_info"][] | select(.scope == "link").local' | \
		head -1
}

bridge_ageing_time_get()
{
	local bridge=$1
	local ageing_time

	# Need to divide by 100 to convert to seconds.
	ageing_time=$(ip -j -d link show dev $bridge \
		      | jq '.[]["linkinfo"]["info_data"]["ageing_time"]')
	echo $((ageing_time / 100))
}

declare -A SYSCTL_ORIG
sysctl_save()
{
	local key=$1; shift

	SYSCTL_ORIG[$key]=$(sysctl -n $key)
}

sysctl_set()
{
	local key=$1; shift
	local value=$1; shift

	sysctl_save "$key"
	sysctl -qw $key="$value"
}

sysctl_restore()
{
	local key=$1; shift

	sysctl -qw $key="${SYSCTL_ORIG[$key]}"
}

forwarding_enable()
{
	sysctl_set net.ipv4.conf.all.forwarding 1
	sysctl_set net.ipv6.conf.all.forwarding 1
}

forwarding_restore()
{
	sysctl_restore net.ipv6.conf.all.forwarding
	sysctl_restore net.ipv4.conf.all.forwarding
}

declare -A MTU_ORIG
mtu_set()
{
	local dev=$1; shift
	local mtu=$1; shift

	MTU_ORIG["$dev"]=$(ip -j link show dev $dev | jq -e '.[].mtu')
	ip link set dev $dev mtu $mtu
}

mtu_restore()
{
	local dev=$1; shift

	ip link set dev $dev mtu ${MTU_ORIG["$dev"]}
}

tc_offload_check()
{
	local num_netifs=${1:-$NUM_NETIFS}

	for ((i = 1; i <= num_netifs; ++i)); do
		ethtool -k ${NETIFS[p$i]} \
			| grep "hw-tc-offload: on" &> /dev/null
		if [[ $? -ne 0 ]]; then
			return 1
		fi
	done

	return 0
}

trap_install()
{
	local dev=$1; shift
	local direction=$1; shift

	# Some devices may not support or need in-hardware trapping of traffic
	# (e.g. the veth pairs that this library creates for non-existent
	# loopbacks). Use continue instead, so that there is a filter in there
	# (some tests check counters), and so that other filters are still
	# processed.
	tc filter add dev $dev $direction pref 1 \
		flower skip_sw action trap 2>/dev/null \
	    || tc filter add dev $dev $direction pref 1 \
		       flower action continue
}

trap_uninstall()
{
	local dev=$1; shift
	local direction=$1; shift

	tc filter del dev $dev $direction pref 1 flower
}

__icmp_capture_add_del()
{
	local add_del=$1; shift
	local pref=$1; shift
	local vsuf=$1; shift
	local tundev=$1; shift
	local filter=$1; shift

	tc filter $add_del dev "$tundev" ingress \
	   proto ip$vsuf pref $pref \
	   flower ip_proto icmp$vsuf $filter \
	   action pass
}

icmp_capture_install()
{
	local tundev=$1; shift
	local filter=$1; shift

	__icmp_capture_add_del add 100 "" "$tundev" "$filter"
}

icmp_capture_uninstall()
{
	local tundev=$1; shift
	local filter=$1; shift

	__icmp_capture_add_del del 100 "" "$tundev" "$filter"
}

icmp6_capture_install()
{
	local tundev=$1; shift
	local filter=$1; shift

	__icmp_capture_add_del add 100 v6 "$tundev" "$filter"
}

icmp6_capture_uninstall()
{
	local tundev=$1; shift
	local filter=$1; shift

	__icmp_capture_add_del del 100 v6 "$tundev" "$filter"
}

__vlan_capture_add_del()
{
	local add_del=$1; shift
	local pref=$1; shift
	local dev=$1; shift
	local filter=$1; shift

	tc filter $add_del dev "$dev" ingress \
	   proto 802.1q pref $pref \
	   flower $filter \
	   action pass
}

vlan_capture_install()
{
	local dev=$1; shift
	local filter=$1; shift

	__vlan_capture_add_del add 100 "$dev" "$filter"
}

vlan_capture_uninstall()
{
	local dev=$1; shift
	local filter=$1; shift

	__vlan_capture_add_del del 100 "$dev" "$filter"
}

__dscp_capture_add_del()
{
	local add_del=$1; shift
	local dev=$1; shift
	local base=$1; shift
	local dscp;

	for prio in {0..7}; do
		dscp=$((base + prio))
		__icmp_capture_add_del $add_del $((dscp + 100)) "" $dev \
				       "skip_hw ip_tos $((dscp << 2))"
	done
}

dscp_capture_install()
{
	local dev=$1; shift
	local base=$1; shift

	__dscp_capture_add_del add $dev $base
}

dscp_capture_uninstall()
{
	local dev=$1; shift
	local base=$1; shift

	__dscp_capture_add_del del $dev $base
}

dscp_fetch_stats()
{
	local dev=$1; shift
	local base=$1; shift

	for prio in {0..7}; do
		local dscp=$((base + prio))
		local t=$(tc_rule_stats_get $dev $((dscp + 100)))
		echo "[$dscp]=$t "
	done
}

matchall_sink_create()
{
	local dev=$1; shift

	tc qdisc add dev $dev clsact
	tc filter add dev $dev ingress \
	   pref 10000 \
	   matchall \
	   action drop
}

cleanup()
{
	pre_cleanup
	defer_scopes_cleanup
}

multipath_eval()
{
	local desc="$1"
	local weight_rp12=$2
	local weight_rp13=$3
	local packets_rp12=$4
	local packets_rp13=$5
	local weights_ratio packets_ratio diff

	RET=0

	if [[ "$weight_rp12" -gt "$weight_rp13" ]]; then
		weights_ratio=$(echo "scale=2; $weight_rp12 / $weight_rp13" \
				| bc -l)
	else
		weights_ratio=$(echo "scale=2; $weight_rp13 / $weight_rp12" \
				| bc -l)
	fi

	if [[ "$packets_rp12" -eq "0" || "$packets_rp13" -eq "0" ]]; then
	       check_err 1 "Packet difference is 0"
	       log_test "Multipath"
	       log_info "Expected ratio $weights_ratio"
	       return
	fi

	if [[ "$weight_rp12" -gt "$weight_rp13" ]]; then
		packets_ratio=$(echo "scale=2; $packets_rp12 / $packets_rp13" \
				| bc -l)
	else
		packets_ratio=$(echo "scale=2; $packets_rp13 / $packets_rp12" \
				| bc -l)
	fi

	diff=$(echo $weights_ratio - $packets_ratio | bc -l)
	diff=${diff#-}

	test "$(echo "$diff / $weights_ratio > 0.15" | bc -l)" -eq 0
	check_err $? "Too large discrepancy between expected and measured ratios"
	log_test "$desc"
	log_info "Expected ratio $weights_ratio Measured ratio $packets_ratio"
}

in_ns()
{
	local name=$1; shift

	ip netns exec $name bash <<-EOF
		NUM_NETIFS=0
		source lib.sh
		$(for a in "$@"; do printf "%q${IFS:0:1}" "$a"; done)
	EOF
}

##############################################################################
# Tests

ping_do()
{
	local if_name=$1
	local dip=$2
	local args=$3
	local vrf_name

	vrf_name=$(master_name_get $if_name)
	ip vrf exec $vrf_name \
		$PING $args $dip -c $PING_COUNT -i 0.1 \
		-w $PING_TIMEOUT &> /dev/null
}

ping_test()
{
	RET=0

	ping_do $1 $2
	check_err $?
	log_test "ping$3"
}

ping_test_fails()
{
	RET=0

	ping_do $1 $2
	check_fail $?
	log_test "ping fails$3"
}

ping6_do()
{
	local if_name=$1
	local dip=$2
	local args=$3
	local vrf_name

	vrf_name=$(master_name_get $if_name)
	ip vrf exec $vrf_name \
		$PING6 $args $dip -c $PING_COUNT -i 0.1 \
		-w $PING_TIMEOUT &> /dev/null
}

ping6_test()
{
	RET=0

	ping6_do $1 $2
	check_err $?
	log_test "ping6$3"
}

ping6_test_fails()
{
	RET=0

	ping6_do $1 $2
	check_fail $?
	log_test "ping6 fails$3"
}

learning_test()
{
	local bridge=$1
	local br_port1=$2	# Connected to `host1_if`.
	local host1_if=$3
	local host2_if=$4
	local mac=de:ad:be:ef:13:37
	local ageing_time

	RET=0

	bridge -j fdb show br $bridge brport $br_port1 \
		| jq -e ".[] | select(.mac == \"$mac\")" &> /dev/null
	check_fail $? "Found FDB record when should not"

	# Disable unknown unicast flooding on `br_port1` to make sure
	# packets are only forwarded through the port after a matching
	# FDB entry was installed.
	bridge link set dev $br_port1 flood off

	ip link set $host1_if promisc on
	tc qdisc add dev $host1_if ingress
	tc filter add dev $host1_if ingress protocol ip pref 1 handle 101 \
		flower dst_mac $mac action drop

	$MZ $host2_if -c 1 -p 64 -b $mac -t ip -q
	sleep 1

	tc -j -s filter show dev $host1_if ingress \
		| jq -e ".[] | select(.options.handle == 101) \
		| select(.options.actions[0].stats.packets == 1)" &> /dev/null
	check_fail $? "Packet reached first host when should not"

	$MZ $host1_if -c 1 -p 64 -a $mac -t ip -q
	sleep 1

	bridge -j fdb show br $bridge brport $br_port1 \
		| jq -e ".[] | select(.mac == \"$mac\")" &> /dev/null
	check_err $? "Did not find FDB record when should"

	$MZ $host2_if -c 1 -p 64 -b $mac -t ip -q
	sleep 1

	tc -j -s filter show dev $host1_if ingress \
		| jq -e ".[] | select(.options.handle == 101) \
		| select(.options.actions[0].stats.packets == 1)" &> /dev/null
	check_err $? "Packet did not reach second host when should"

	# Wait for 10 seconds after the ageing time to make sure FDB
	# record was aged-out.
	ageing_time=$(bridge_ageing_time_get $bridge)
	sleep $((ageing_time + 10))

	bridge -j fdb show br $bridge brport $br_port1 \
		| jq -e ".[] | select(.mac == \"$mac\")" &> /dev/null
	check_fail $? "Found FDB record when should not"

	bridge link set dev $br_port1 learning off

	$MZ $host1_if -c 1 -p 64 -a $mac -t ip -q
	sleep 1

	bridge -j fdb show br $bridge brport $br_port1 \
		| jq -e ".[] | select(.mac == \"$mac\")" &> /dev/null
	check_fail $? "Found FDB record when should not"

	bridge link set dev $br_port1 learning on

	tc filter del dev $host1_if ingress protocol ip pref 1 handle 101 flower
	tc qdisc del dev $host1_if ingress
	ip link set $host1_if promisc off

	bridge link set dev $br_port1 flood on

	log_test "FDB learning"
}

flood_test_do()
{
	local should_flood=$1
	local mac=$2
	local ip=$3
	local host1_if=$4
	local host2_if=$5
	local err=0

	# Add an ACL on `host2_if` which will tell us whether the packet
	# was flooded to it or not.
	ip link set $host2_if promisc on
	tc qdisc add dev $host2_if ingress
	tc filter add dev $host2_if ingress protocol ip pref 1 handle 101 \
		flower dst_mac $mac action drop

	$MZ $host1_if -c 1 -p 64 -b $mac -B $ip -t ip -q
	sleep 1

	tc -j -s filter show dev $host2_if ingress \
		| jq -e ".[] | select(.options.handle == 101) \
		| select(.options.actions[0].stats.packets == 1)" &> /dev/null
	if [[ $? -ne 0 && $should_flood == "true" || \
	      $? -eq 0 && $should_flood == "false" ]]; then
		err=1
	fi

	tc filter del dev $host2_if ingress protocol ip pref 1 handle 101 flower
	tc qdisc del dev $host2_if ingress
	ip link set $host2_if promisc off

	return $err
}

flood_unicast_test()
{
	local br_port=$1
	local host1_if=$2
	local host2_if=$3
	local mac=de:ad:be:ef:13:37
	local ip=192.0.2.100

	RET=0

	bridge link set dev $br_port flood off

	flood_test_do false $mac $ip $host1_if $host2_if
	check_err $? "Packet flooded when should not"

	bridge link set dev $br_port flood on

	flood_test_do true $mac $ip $host1_if $host2_if
	check_err $? "Packet was not flooded when should"

	log_test "Unknown unicast flood"
}

flood_multicast_test()
{
	local br_port=$1
	local host1_if=$2
	local host2_if=$3
	local mac=01:00:5e:00:00:01
	local ip=239.0.0.1

	RET=0

	bridge link set dev $br_port mcast_flood off

	flood_test_do false $mac $ip $host1_if $host2_if
	check_err $? "Packet flooded when should not"

	bridge link set dev $br_port mcast_flood on

	flood_test_do true $mac $ip $host1_if $host2_if
	check_err $? "Packet was not flooded when should"

	log_test "Unregistered multicast flood"
}

flood_test()
{
	# `br_port` is connected to `host2_if`
	local br_port=$1
	local host1_if=$2
	local host2_if=$3

	flood_unicast_test $br_port $host1_if $host2_if
	flood_multicast_test $br_port $host1_if $host2_if
}

__start_traffic()
{
	local pktsize=$1; shift
	local proto=$1; shift
	local h_in=$1; shift    # Where the traffic egresses the host
	local sip=$1; shift
	local dip=$1; shift
	local dmac=$1; shift
	local -a mz_args=("$@")

	$MZ $h_in -p $pktsize -A $sip -B $dip -c 0 \
		-a own -b $dmac -t "$proto" -q "${mz_args[@]}" &
	sleep 1
}

start_traffic_pktsize()
{
	local pktsize=$1; shift
	local h_in=$1; shift
	local sip=$1; shift
	local dip=$1; shift
	local dmac=$1; shift
	local -a mz_args=("$@")

	__start_traffic $pktsize udp "$h_in" "$sip" "$dip" "$dmac" \
			"${mz_args[@]}"
}

start_tcp_traffic_pktsize()
{
	local pktsize=$1; shift
	local h_in=$1; shift
	local sip=$1; shift
	local dip=$1; shift
	local dmac=$1; shift
	local -a mz_args=("$@")

	__start_traffic $pktsize tcp "$h_in" "$sip" "$dip" "$dmac" \
			"${mz_args[@]}"
}

start_traffic()
{
	local h_in=$1; shift
	local sip=$1; shift
	local dip=$1; shift
	local dmac=$1; shift
	local -a mz_args=("$@")

	start_traffic_pktsize 8000 "$h_in" "$sip" "$dip" "$dmac" \
			      "${mz_args[@]}"
}

start_tcp_traffic()
{
	local h_in=$1; shift
	local sip=$1; shift
	local dip=$1; shift
	local dmac=$1; shift
	local -a mz_args=("$@")

	start_tcp_traffic_pktsize 8000 "$h_in" "$sip" "$dip" "$dmac" \
				  "${mz_args[@]}"
}

stop_traffic()
{
	local pid=${1-%%}; shift

	kill_process "$pid"
}

declare -A cappid
declare -A capfile
declare -A capout

tcpdump_start()
{
	local if_name=$1; shift
	local ns=$1; shift

	capfile[$if_name]=$(mktemp)
	capout[$if_name]=$(mktemp)

	if [ -z $ns ]; then
		ns_cmd=""
	else
		ns_cmd="ip netns exec ${ns}"
	fi

	if [ -z $SUDO_USER ] ; then
		capuser=""
	else
		capuser="-Z $SUDO_USER"
	fi

	$ns_cmd tcpdump $TCPDUMP_EXTRA_FLAGS -e -n -Q in -i $if_name \
		-s 65535 -B 32768 $capuser -w ${capfile[$if_name]} \
		> "${capout[$if_name]}" 2>&1 &
	cappid[$if_name]=$!

	sleep 1
}

tcpdump_stop()
{
	local if_name=$1
	local pid=${cappid[$if_name]}

	$ns_cmd kill "$pid" && wait "$pid"
	sleep 1
}

tcpdump_cleanup()
{
	local if_name=$1

	rm ${capfile[$if_name]} ${capout[$if_name]}
}

tcpdump_show()
{
	local if_name=$1

	tcpdump -e -n -r ${capfile[$if_name]} 2>&1
}

# return 0 if the packet wasn't seen on host2_if or 1 if it was
mcast_packet_test()
{
	local mac=$1
	local src_ip=$2
	local ip=$3
	local host1_if=$4
	local host2_if=$5
	local seen=0
	local tc_proto="ip"
	local mz_v6arg=""

	# basic check to see if we were passed an IPv4 address, if not assume IPv6
	if [[ ! $ip =~ ^[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}$ ]]; then
		tc_proto="ipv6"
		mz_v6arg="-6"
	fi

	# Add an ACL on `host2_if` which will tell us whether the packet
	# was received by it or not.
	tc qdisc add dev $host2_if ingress
	tc filter add dev $host2_if ingress protocol $tc_proto pref 1 handle 101 \
		flower ip_proto udp dst_mac $mac action drop

	$MZ $host1_if $mz_v6arg -c 1 -p 64 -b $mac -A $src_ip -B $ip -t udp "dp=4096,sp=2048" -q
	sleep 1

	tc -j -s filter show dev $host2_if ingress \
		| jq -e ".[] | select(.options.handle == 101) \
		| select(.options.actions[0].stats.packets == 1)" &> /dev/null
	if [[ $? -eq 0 ]]; then
		seen=1
	fi

	tc filter del dev $host2_if ingress protocol $tc_proto pref 1 handle 101 flower
	tc qdisc del dev $host2_if ingress

	return $seen
}

brmcast_check_sg_entries()
{
	local report=$1; shift
	local slist=("$@")
	local sarg=""

	for src in "${slist[@]}"; do
		sarg="${sarg} and .source_list[].address == \"$src\""
	done
	bridge -j -d -s mdb show dev br0 \
		| jq -e ".[].mdb[] | \
			 select(.grp == \"$TEST_GROUP\" and .source_list != null $sarg)" &>/dev/null
	check_err $? "Wrong *,G entry source list after $report report"

	for sgent in "${slist[@]}"; do
		bridge -j -d -s mdb show dev br0 \
			| jq -e ".[].mdb[] | \
				 select(.grp == \"$TEST_GROUP\" and .src == \"$sgent\")" &>/dev/null
		check_err $? "Missing S,G entry ($sgent, $TEST_GROUP)"
	done
}

brmcast_check_sg_fwding()
{
	local should_fwd=$1; shift
	local sources=("$@")

	for src in "${sources[@]}"; do
		local retval=0

		mcast_packet_test $TEST_GROUP_MAC $src $TEST_GROUP $h2 $h1
		retval=$?
		if [ $should_fwd -eq 1 ]; then
			check_fail $retval "Didn't forward traffic from S,G ($src, $TEST_GROUP)"
		else
			check_err $retval "Forwarded traffic for blocked S,G ($src, $TEST_GROUP)"
		fi
	done
}

brmcast_check_sg_state()
{
	local is_blocked=$1; shift
	local sources=("$@")
	local should_fail=1

	if [ $is_blocked -eq 1 ]; then
		should_fail=0
	fi

	for src in "${sources[@]}"; do
		bridge -j -d -s mdb show dev br0 \
			| jq -e ".[].mdb[] | \
				 select(.grp == \"$TEST_GROUP\" and .source_list != null) |
				 .source_list[] |
				 select(.address == \"$src\") |
				 select(.timer == \"0.00\")" &>/dev/null
		check_err_fail $should_fail $? "Entry $src has zero timer"

		bridge -j -d -s mdb show dev br0 \
			| jq -e ".[].mdb[] | \
				 select(.grp == \"$TEST_GROUP\" and .src == \"$src\" and \
				 .flags[] == \"blocked\")" &>/dev/null
		check_err_fail $should_fail $? "Entry $src has blocked flag"
	done
}

mc_join()
{
	local if_name=$1
	local group=$2
	local vrf_name=$(master_name_get $if_name)

	# We don't care about actual reception, just about joining the
	# IP multicast group and adding the L2 address to the device's
	# MAC filtering table
	ip vrf exec $vrf_name \
		mreceive -g $group -I $if_name > /dev/null 2>&1 &
	mreceive_pid=$!

	sleep 1
}

mc_leave()
{
	kill "$mreceive_pid" && wait "$mreceive_pid"
}

mc_send()
{
	local if_name=$1
	local groups=$2
	local vrf_name=$(master_name_get $if_name)

	ip vrf exec $vrf_name \
		msend -g $groups -I $if_name -c 1 > /dev/null 2>&1
}

adf_mcd_start()
{
	local ifs=("$@")

	local table_name="$MCD_TABLE_NAME"
	local smcroutedir
	local pid
	local if
	local i

	check_command "$MCD" || return 1
	check_command "$MC_CLI" || return 1

	smcroutedir=$(mktemp -d)
	defer rm -rf "$smcroutedir"

	for ((i = 1; i <= NUM_NETIFS; ++i)); do
		echo "phyint ${NETIFS[p$i]} enable" >> \
			"$smcroutedir/$table_name.conf"
	done

	for if in "${ifs[@]}"; do
		if ! ip_link_has_flag "$if" MULTICAST; then
			ip link set dev "$if" multicast on
			defer ip link set dev "$if" multicast off
		fi

		echo "phyint $if enable" >> \
			"$smcroutedir/$table_name.conf"
	done

	"$MCD" -N -I "$table_name" -f "$smcroutedir/$table_name.conf" \
		-P "$smcroutedir/$table_name.pid"
	busywait "$BUSYWAIT_TIMEOUT" test -e "$smcroutedir/$table_name.pid"
	pid=$(cat "$smcroutedir/$table_name.pid")
	defer kill_process "$pid"
}

mc_cli()
{
	local table_name="$MCD_TABLE_NAME"

        "$MC_CLI" -I "$table_name" "$@"
}

start_ip_monitor()
{
	local mtype=$1; shift
	local ip=${1-ip}; shift

	# start the monitor in the background
	tmpfile=`mktemp /var/run/nexthoptestXXX`
	mpid=`($ip monitor $mtype > $tmpfile & echo $!) 2>/dev/null`
	sleep 0.2
	echo "$mpid $tmpfile"
}

stop_ip_monitor()
{
	local mpid=$1; shift
	local tmpfile=$1; shift
	local el=$1; shift
	local what=$1; shift

	sleep 0.2
	kill $mpid
	local lines=`grep '^\w' $tmpfile | wc -l`
	test $lines -eq $el
	check_err $? "$what: $lines lines of events, expected $el"
	rm -rf $tmpfile
}

hw_stats_monitor_test()
{
	local dev=$1; shift
	local type=$1; shift
	local make_suitable=$1; shift
	local make_unsuitable=$1; shift
	local ip=${1-ip}; shift

	RET=0

	# Expect a notification about enablement.
	local ipmout=$(start_ip_monitor stats "$ip")
	$ip stats set dev $dev ${type}_stats on
	stop_ip_monitor $ipmout 1 "${type}_stats enablement"

	# Expect a notification about offload.
	local ipmout=$(start_ip_monitor stats "$ip")
	$make_suitable
	stop_ip_monitor $ipmout 1 "${type}_stats installation"

	# Expect a notification about loss of offload.
	local ipmout=$(start_ip_monitor stats "$ip")
	$make_unsuitable
	stop_ip_monitor $ipmout 1 "${type}_stats deinstallation"

	# Expect a notification about disablement
	local ipmout=$(start_ip_monitor stats "$ip")
	$ip stats set dev $dev ${type}_stats off
	stop_ip_monitor $ipmout 1 "${type}_stats disablement"

	log_test "${type}_stats notifications"
}

ipv4_to_bytes()
{
	local IP=$1; shift

	printf '%02x:' ${IP//./ } |
	    sed 's/:$//'
}

# Convert a given IPv6 address, `IP' such that the :: token, if present, is
# expanded, and each 16-bit group is padded with zeroes to be 4 hexadecimal
# digits. An optional `BYTESEP' parameter can be given to further separate
# individual bytes of each 16-bit group.
expand_ipv6()
{
	local IP=$1; shift
	local bytesep=$1; shift

	local cvt_ip=${IP/::/_}
	local colons=${cvt_ip//[^:]/}
	local allcol=:::::::
	# IP where :: -> the appropriate number of colons:
	local allcol_ip=${cvt_ip/_/${allcol:${#colons}}}

	echo $allcol_ip | tr : '\n' |
	    sed s/^/0000/ |
	    sed 's/.*\(..\)\(..\)/\1'"$bytesep"'\2/' |
	    tr '\n' : |
	    sed 's/:$//'
}

ipv6_to_bytes()
{
	local IP=$1; shift

	expand_ipv6 "$IP" :
}

u16_to_bytes()
{
	local u16=$1; shift

	printf "%04x" $u16 | sed 's/^/000/;s/^.*\(..\)\(..\)$/\1:\2/'
}

# Given a mausezahn-formatted payload (colon-separated bytes given as %02x),
# possibly with a keyword CHECKSUM stashed where a 16-bit checksum should be,
# calculate checksum as per RFC 1071, assuming the CHECKSUM field (if any)
# stands for 00:00.
payload_template_calc_checksum()
{
	local payload=$1; shift

	(
	    # Set input radix.
	    echo "16i"
	    # Push zero for the initial checksum.
	    echo 0

	    # Pad the payload with a terminating 00: in case we get an odd
	    # number of bytes.
	    echo "${payload%:}:00:" |
		sed 's/CHECKSUM/00:00/g' |
		tr '[:lower:]' '[:upper:]' |
		# Add the word to the checksum.
		sed 's/\(..\):\(..\):/\1\2+\n/g' |
		# Strip the extra odd byte we pushed if left unconverted.
		sed 's/\(..\):$//'

	    echo "10000 ~ +"	# Calculate and add carry.
	    echo "FFFF r - p"	# Bit-flip and print.
	) |
	    dc |
	    tr '[:upper:]' '[:lower:]'
}

payload_template_expand_checksum()
{
	local payload=$1; shift
	local checksum=$1; shift

	local ckbytes=$(u16_to_bytes $checksum)

	echo "$payload" | sed "s/CHECKSUM/$ckbytes/g"
}

payload_template_nbytes()
{
	local payload=$1; shift

	payload_template_expand_checksum "${payload%:}" 0 |
		sed 's/:/\n/g' | wc -l
}

igmpv3_is_in_get()
{
	local GRP=$1; shift
	local sources=("$@")

	local igmpv3
	local nsources=$(u16_to_bytes ${#sources[@]})

	# IS_IN ( $sources )
	igmpv3=$(:
		)"22:"$(			: Type - Membership Report
		)"00:"$(			: Reserved
		)"CHECKSUM:"$(			: Checksum
		)"00:00:"$(			: Reserved
		)"00:01:"$(			: Number of Group Records
		)"01:"$(			: Record Type - IS_IN
		)"00:"$(			: Aux Data Len
		)"${nsources}:"$(		: Number of Sources
		)"$(ipv4_to_bytes $GRP):"$(	: Multicast Address
		)"$(for src in "${sources[@]}"; do
			ipv4_to_bytes $src
			echo -n :
		    done)"$(			: Source Addresses
		)
	local checksum=$(payload_template_calc_checksum "$igmpv3")

	payload_template_expand_checksum "$igmpv3" $checksum
}

igmpv2_leave_get()
{
	local GRP=$1; shift

	local payload=$(:
		)"17:"$(			: Type - Leave Group
		)"00:"$(			: Max Resp Time - not meaningful
		)"CHECKSUM:"$(			: Checksum
		)"$(ipv4_to_bytes $GRP)"$(	: Group Address
		)
	local checksum=$(payload_template_calc_checksum "$payload")

	payload_template_expand_checksum "$payload" $checksum
}

mldv2_is_in_get()
{
	local SIP=$1; shift
	local GRP=$1; shift
	local sources=("$@")

	local hbh
	local icmpv6
	local nsources=$(u16_to_bytes ${#sources[@]})

	hbh=$(:
		)"3a:"$(			: Next Header - ICMPv6
		)"00:"$(			: Hdr Ext Len
		)"00:00:00:00:00:00:"$(		: Options and Padding
		)

	icmpv6=$(:
		)"8f:"$(			: Type - MLDv2 Report
		)"00:"$(			: Code
		)"CHECKSUM:"$(			: Checksum
		)"00:00:"$(			: Reserved
		)"00:01:"$(			: Number of Group Records
		)"01:"$(			: Record Type - IS_IN
		)"00:"$(			: Aux Data Len
		)"${nsources}:"$(		: Number of Sources
		)"$(ipv6_to_bytes $GRP):"$(	: Multicast address
		)"$(for src in "${sources[@]}"; do
			ipv6_to_bytes $src
			echo -n :
		    done)"$(			: Source Addresses
		)

	local len=$(u16_to_bytes $(payload_template_nbytes $icmpv6))
	local sudohdr=$(:
		)"$(ipv6_to_bytes $SIP):"$(	: SIP
		)"$(ipv6_to_bytes $GRP):"$(	: DIP is multicast address
	        )"${len}:"$(			: Upper-layer length
	        )"00:3a:"$(			: Zero and next-header
	        )
	local checksum=$(payload_template_calc_checksum ${sudohdr}${icmpv6})

	payload_template_expand_checksum "$hbh$icmpv6" $checksum
}

mldv1_done_get()
{
	local SIP=$1; shift
	local GRP=$1; shift

	local hbh
	local icmpv6

	hbh=$(:
		)"3a:"$(			: Next Header - ICMPv6
		)"00:"$(			: Hdr Ext Len
		)"00:00:00:00:00:00:"$(		: Options and Padding
		)

	icmpv6=$(:
		)"84:"$(			: Type - MLDv1 Done
		)"00:"$(			: Code
		)"CHECKSUM:"$(			: Checksum
		)"00:00:"$(			: Max Resp Delay - not meaningful
		)"00:00:"$(			: Reserved
		)"$(ipv6_to_bytes $GRP):"$(	: Multicast address
		)

	local len=$(u16_to_bytes $(payload_template_nbytes $icmpv6))
	local sudohdr=$(:
		)"$(ipv6_to_bytes $SIP):"$(	: SIP
		)"$(ipv6_to_bytes $GRP):"$(	: DIP is multicast address
	        )"${len}:"$(			: Upper-layer length
	        )"00:3a:"$(			: Zero and next-header
	        )
	local checksum=$(payload_template_calc_checksum ${sudohdr}${icmpv6})

	payload_template_expand_checksum "$hbh$icmpv6" $checksum
}

bail_on_lldpad()
{
	local reason1="$1"; shift
	local reason2="$1"; shift
	local caller=${FUNCNAME[1]}
	local src=${BASH_SOURCE[1]}

	if systemctl is-active --quiet lldpad; then

		cat >/dev/stderr <<-EOF
		WARNING: lldpad is running

			lldpad will likely $reason1, and this test will
			$reason2. Both are not supported at the same time,
			one of them is arbitrarily going to overwrite the
			other. That will cause spurious failures (or, unlikely,
			passes) of this test.
		EOF

		if [[ -z $ALLOW_LLDPAD ]]; then
			cat >/dev/stderr <<-EOF

				If you want to run the test anyway, please set
				an environment variable ALLOW_LLDPAD to a
				non-empty string.
			EOF
			log_test_skip $src:$caller
			exit $EXIT_STATUS
		else
			return
		fi
	fi
}

absval()
{
	local v=$1; shift

	echo $((v > 0 ? v : -v))
}

has_unicast_flt()
{
	local dev=$1; shift
	local mac_addr=$(mac_get $dev)
	local tmp=$(ether_addr_to_u64 $mac_addr)
	local promisc

	ip link set $dev up
	ip link add link $dev name macvlan-tmp type macvlan mode private
	ip link set macvlan-tmp address $(u64_to_ether_addr $((tmp + 1)))
	ip link set macvlan-tmp up

	promisc=$(ip -j -d link show dev $dev | jq -r '.[].promiscuity')

	ip link del macvlan-tmp

	[[ $promisc == 1 ]] && echo "no" || echo "yes"
}
