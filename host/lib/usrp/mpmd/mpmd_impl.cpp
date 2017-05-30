//
// Copyright 2017 Ettus Research (National Instruments)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "mpmd_impl.hpp"
#include "rpc_block_ctrl.hpp"
#include <../device3/device3_impl.hpp>
#include <uhd/exception.hpp>
#include <uhd/property_tree.hpp>
#include <uhd/transport/if_addrs.hpp>
#include <uhd/transport/udp_simple.hpp>
#include <uhd/transport/udp_zero_copy.hpp>
#include <uhd/utils/static.hpp>
#include <uhd/utils/tasks.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/make_shared.hpp>
#include <boost/thread.hpp>
#include <memory>
#include <mutex>
#include <random>
#include <string>

using namespace uhd;

mpmd_mboard_impl::mpmd_mboard_impl(const std::string& addr)
    : rpc(uhd::rpc_client::make(addr, MPM_RPC_PORT))
{
    UHD_LOG_TRACE("MPMD", "Initializing mboard, IP address: " << addr);
    std::map<std::string, std::string> _dev_info =
        rpc->call<dev_info>("get_device_info");
    device_info =
        dict<std::string, std::string>(_dev_info.begin(), _dev_info.end());
    // Get initial claim on mboard
    _rpc_token = rpc->call<std::string>("claim", "UHD - Session 01"); // TODO make this configurable with device_addr, and provide better defaults
    if (_rpc_token.empty()){
        throw uhd::value_error("mpmd device claiming failed!");
    }
    rpc->set_token(_rpc_token);
    _claimer_task = task::make([this] {
        if (not this->claim()) {
            throw uhd::value_error("mpmd device reclaiming loop failed!");
        };
        boost::this_thread::sleep_for(boost::chrono::milliseconds(1000));
    });
    // std::vector<std::string> data_ifaces =
    //     rpc.call<std::vector<std::string>>("get_interfaces", _rpc_token);

    // discover path to device and tell MPM our MAC address seen at the data
    // interfaces
    // move this into make_transport
    //for (const auto& iface : data_ifaces) {
        //std::vector<std::string> addrs = rpc.call<std::vector<std::string>>(
            //"get_interface_addrs", _rpc_token, iface);
        //for (const auto& iface_addr : addrs) {
            //if (rpc_client(iface_addr, MPM_RPC_PORT)
                    //.call<bool>("probe_interface", _rpc_token)) {
                //data_interfaces.emplace(iface, iface_addr);
                //break;
            //}
        //}
    //}
}

uhd::sid_t mpmd_mboard_impl::allocate_sid(const uint16_t port,
                                          const uhd::sid_t address,
                                          const uint32_t xbar_src_addr,
                                          const uint32_t xbar_src_port)
{
    const uint32_t sid = rpc->call_with_token<uint32_t>(
        "allocate_sid",
        port, address.get(), xbar_src_addr, xbar_src_port
    );
    return sid;
}

mpmd_mboard_impl::~mpmd_mboard_impl() {}

mpmd_mboard_impl::uptr mpmd_mboard_impl::make(const std::string& addr)
{
    mpmd_mboard_impl::uptr mb =
        mpmd_mboard_impl::uptr(new mpmd_mboard_impl(addr));
    // implicit move
    return mb;
}

bool mpmd_mboard_impl::claim()
{
    return rpc->call_with_token<bool>("reclaim");
}

mpmd_impl::mpmd_impl(const device_addr_t& device_addr)
    : usrp::device3_impl()
    , _device_addr(device_addr)
    , _sid_framer(0)
{
    UHD_LOGGER_INFO("MPMD")
        << "MPMD initialization sequence. Device args: "
        << device_addr.to_string();
    const device_addrs_t device_args = separate_device_addr(device_addr);
    _mb.reserve(device_args.size());
    for (size_t mb_i = 0; mb_i < device_args.size(); ++mb_i) {
        _mb.push_back(setup_mb(mb_i, device_args[mb_i]));
    }


    // TODO read this from the device info
    _tree->create<std::string>("/name").set("MPMD - Series device");

    try {
        enumerate_rfnoc_blocks(
          0,
          3, /* num blocks */ // TODO don't hardcode
          3, /* base port */  // TODO don't hardcode
          uhd::sid_t(0x0200), // TODO don't hardcode
          device_addr
        );
    } catch (const std::exception &ex) {
        UHD_LOGGER_ERROR("MPMD")
            << "Failure during device initialization: "
            << ex.what();
        throw uhd::runtime_error("Failed to run enumerate_rfnoc_blocks()");
    }

    for (const auto &block_ctrl: _rfnoc_block_ctrl) {
        auto rpc_block_id = block_ctrl->get_block_id();
        if (has_block<uhd::rfnoc::rpc_block_ctrl>(block_ctrl->get_block_id())) {
            const size_t mboard_idx = rpc_block_id.get_device_no();
            UHD_LOGGER_DEBUG("MPMD")
                << "Adding RPC access to block: " << rpc_block_id
                << " Extra device args: " << device_args[mboard_idx].to_string()
            ;
            get_block_ctrl<uhd::rfnoc::rpc_block_ctrl>(rpc_block_id)
                ->set_rpc_client(_mb[mboard_idx]->rpc, device_args[mboard_idx]);
        }
    }
}

mpmd_impl::~mpmd_impl() {}

mpmd_mboard_impl::uptr mpmd_impl::setup_mb(const size_t mb_i,
                                           const uhd::device_addr_t& dev_addr)
{
    const fs_path mb_path = "/mboards/" + std::to_string(mb_i);
    mpmd_mboard_impl::uptr mb = mpmd_mboard_impl::make(dev_addr["addr"]);
    mb->initialization_done = false;
    std::vector<std::string> addrs;
    const std::string eth0_addr = dev_addr["addr"];
    _tree->create<std::string>(mb_path / "name")
        .set(mb->device_info.get("type", ""));
    _tree->create<std::string>(mb_path / "serial")
        .set(mb->device_info.get("serial", ""));
    _tree->create<std::string>(mb_path / "connection")
        .set(mb->device_info.get("connection", "remote"));

    for (const std::string& key : dev_addr.keys()) {
        if (key.find("recv") != std::string::npos)
            mb->recv_args[key] = dev_addr[key];
        if (key.find("send") != std::string::npos)
            mb->send_args[key] = dev_addr[key];
    }

    // Do real MTU discovery (something similar like X300 but with MPM)

    _tree->create<size_t>(mb_path / "mtu/recv").set(1500);
    _tree->create<size_t>(mb_path / "mtu/send").set(1500);
    _tree->create<size_t>(mb_path / "link_max_rate").set(1e9 / 8);

    // query more information about FPGA/MPM

    // Call init on periph_manager, this will init the dboards/mboard, maybe
    // even selfcal and everything

    // Query time/clock sources on mboards/dboards
    // Throw rpc calls with boost bind into the property tree?

    // Query rfnoc blocks on the device (MPM may know about them?)

    // call enumerate rfnoc_blocks on the device

    // configure radio?

    // implicit move
    return mb;
}


// TODO this does not consider the liberio use case!
uhd::device_addr_t mpmd_impl::get_rx_hints(size_t /* mb_index */)
{
    //device_addr_t rx_hints = _mb[mb_index].recv_args;
    device_addr_t rx_hints; // TODO don't ignore what the user tells us
    // (default to a large recv buff)
    if (not rx_hints.has_key("recv_buff_size"))
    {
        //For the ethernet transport, the buffer has to be set before creating
        //the transport because it is independent of the frame size and # frames
        //For nirio, the buffer size is not configurable by the user
        #if defined(UHD_PLATFORM_MACOS) || defined(UHD_PLATFORM_BSD)
            //limit buffer resize on macos or it will error
            rx_hints["recv_buff_size"] = boost::lexical_cast<std::string>(MPMD_RX_SW_BUFF_SIZE_ETH_MACOS);
        #elif defined(UHD_PLATFORM_LINUX) || defined(UHD_PLATFORM_WIN32)
            //set to half-a-second of buffering at max rate
            rx_hints["recv_buff_size"] = boost::lexical_cast<std::string>(MPMD_RX_SW_BUFF_SIZE_ETH);
        #endif
    }
    return rx_hints;
}


// frame_size_t determine_max_frame_size(const std::string &addr,
//                                       const frame_size_t &user_frame_size){
//     transport::udp_simple::sptr udp =
//     transport::udp_simple::make_connected(addr,
//                                                                             std::to_string(MPM_DISCOVERY_PORT));
//     std::vector<uint8_t> buffer(std::max(user_frame_size.rec))
// }
// Everything fake below here

both_xports_t mpmd_impl::make_transport(const sid_t& address,
                                        usrp::device3_impl::xport_type_t xport_type,
                                        const uhd::device_addr_t& args)
{
    //const size_t mb_index = address.get_dst_addr();
    size_t mb_index = 0;

    both_xports_t xports;
    xports.endianness = uhd::ENDIANNESS_BIG;
    const uhd::device_addr_t& xport_args = (xport_type == CTRL) ? uhd::device_addr_t() : args;
    transport::zero_copy_xport_params default_buff_args;

    /*
    std::cout << address << std::endl;
    std::cout << address.get_src_addr() << std::endl;
    std::cout << address.get_dst_addr() << std::endl;
    */

    std::string interface_addr = _device_addr["addr"];
    const uint32_t xbar_src_addr = address.get_src_addr();
    const uint32_t xbar_src_dst = 0;

    default_buff_args.send_frame_size = 8000;
    default_buff_args.recv_frame_size = 8000;
    default_buff_args.num_recv_frames = 32;
    default_buff_args.num_send_frames = 32;
    // hardcode frame size for now

    transport::udp_zero_copy::buff_params buff_params;
    auto recv = transport::udp_zero_copy::make(
        interface_addr,
        BOOST_STRINGIZE(49153),
        default_buff_args,
        buff_params,
        xport_args);
    uint16_t port  = recv->get_local_port();

    xports.send_sid = _mb[mb_index]->allocate_sid(port, address, xbar_src_addr, xbar_src_dst);
    xports.recv_sid = xports.send_sid.reversed();


    xports.recv_buff_size = buff_params.recv_buff_size;
    xports.send_buff_size = buff_params.send_buff_size;

    xports.recv = recv;
    xports.send = xports.recv;

    return xports;
}

device_addrs_t mpmd_find_with_addr(const device_addr_t& hint_)
{
    transport::udp_simple::sptr comm = transport::udp_simple::make_broadcast(
        hint_["addr"], std::to_string(MPM_DISCOVERY_PORT));
    comm->send(
        boost::asio::buffer(&MPM_DISCOVERY_CMD, sizeof(MPM_DISCOVERY_CMD)));
    device_addrs_t addrs;
    while (true) {
        char buff[4096] = {};
        const size_t nbytes = comm->recv(boost::asio::buffer(buff), 0.050);
        if (nbytes == 0) {
            break;
        }
        const char* reply = (const char*)buff;
        std::string reply_string = std::string(reply);
        std::vector<std::string> result;
        boost::algorithm::split(result, reply_string,
                                [](const char& in) { return in == ';'; },
                                boost::token_compress_on);
        if (result.empty()) {
            continue;
        }
        // who else is reposending to our request !?
        if (result[0] != "USRP-MPM") {
            continue;
        }
        const std::string recv_addr = comm->get_recv_addr();

        // remove external iface addrs if executed directly on device
        bool external_iface = false;
        for (const auto& addr : transport::get_if_addrs()) {
            if ((addr.inet == comm->get_recv_addr()) &&
                recv_addr !=
                    boost::asio::ip::address_v4::loopback().to_string()) {
                external_iface = true;
            }
        }
        if (external_iface) {
            continue;
        }
        device_addr_t new_addr;
        new_addr["addr"] = recv_addr;
        new_addr["type"] = "mpmd"; // hwd will overwrite this
        // remove ident string and put other informations into device_addr dict
        result.erase(result.begin());
        // parse key-value pairs in the discovery string and add them to the
        // device_addr
        for (const auto& el : result) {
            std::vector<std::string> value;
            boost::algorithm::split(value, el,
                                    [](const char& in) { return in == '='; },
                                    boost::token_compress_on);
            new_addr[value[0]] = value[1];
        }
        addrs.push_back(new_addr);
    }
    return addrs;
};

device_addrs_t mpmd_find(const device_addr_t& hint_)
{
    // handle cases:
    //
    //  - empty hint
    //  - multiple addrs
    //  - single addr

    device_addrs_t hints = separate_device_addr(hint_);
    // either hints has:
    // multiple entries
    //   -> search for multiple devices and join them back into one
    //   device_addr_t
    // one entry with addr:
    //   -> search for one device with this addr
    // one
    // multiple addrs
    if (hints.size() > 1) {
        device_addrs_t found_devices;
        found_devices.reserve(hints.size());
        for (const auto& hint : hints) {
            if (not hint.has_key("addr")) { // maybe allow other attributes as well
                return device_addrs_t();
            }
            device_addrs_t reply_addrs = mpmd_find_with_addr(hint);
            if (reply_addrs.size() > 1) {
                throw uhd::value_error(
                    str(boost::format("Could not resolve device hint \"%s\" to "
                                      "a single device.") %
                        hint.to_string()));
            } else if (reply_addrs.empty()) {
                return device_addrs_t();
            }
            found_devices.push_back(reply_addrs[0]);
        }
        return device_addrs_t(1, combine_device_addrs(found_devices));
    }
    hints.resize(1);
    device_addr_t hint = hints[0];
    device_addrs_t addrs;

    if (hint.has_key("addr")) {
        // is this safe?
        return mpmd_find_with_addr(hint);
    }

    for (const transport::if_addrs_t& if_addr : transport::get_if_addrs()) {
        device_addr_t new_hint = hint;
        new_hint["addr"] = if_addr.bcast;

        device_addrs_t reply_addrs = mpmd_find_with_addr(new_hint);
        addrs.insert(addrs.begin(), reply_addrs.begin(), reply_addrs.end());
    }
    return addrs;
}

static device::sptr mpmd_make(const device_addr_t& device_addr)
{
    return device::sptr(boost::make_shared<mpmd_impl>(device_addr));
}

UHD_STATIC_BLOCK(register_mpmd_device)
{
    device::register_device(&mpmd_find, &mpmd_make, device::USRP);
}
// vim: sw=4 expandtab:
