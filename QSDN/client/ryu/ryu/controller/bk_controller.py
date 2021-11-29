# Copyright (C) 2011, 2012 Nippon Telegraph and Telephone Corporation.
# Copyright (C) 2011, 2012 Isaku Yamahata <yamahata at valinux co jp>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
# implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""
The main component of OpenFlow controller.

- Handle connections from switches
- Generate and route events to appropriate entities like Ryu applications

"""

import contextlib
import logging
import random

import socket

from socket import IPPROTO_TCP
from socket import TCP_NODELAY
from socket import SHUT_WR
from socket import timeout as SocketTimeout
import ssl
import sys

from ryu import cfg
from ryu.lib import hub
from ryu.lib.hub import StreamServer

import ryu.base.app_manager

from ryu.ofproto import ofproto_common
from ryu.ofproto import ofproto_parser
from ryu.ofproto import ofproto_protocol
from ryu.ofproto import ofproto_v1_0
from ryu.ofproto import nx_match

from ryu.controller import ofp_event
from ryu.controller.handler import HANDSHAKE_DISPATCHER, DEAD_DISPATCHER

from ryu.lib.dpid import dpid_to_str
from ryu.lib import ip

from ryu.lib.packet import packet
from ryu.lib.packet import ethernet
from ryu.lib.packet import ether_types

LOG = logging.getLogger('ryu.controller.controller')

DEFAULT_OFP_HOST = '0.0.0.0'
DEFAULT_OFP_SW_CON_INTERVAL = 1

CONF = cfg.CONF
CONF.register_cli_opts([
    cfg.StrOpt('ofp-listen-host', default=DEFAULT_OFP_HOST,
               help='openflow listen host (default %s)' % DEFAULT_OFP_HOST),
    cfg.IntOpt('ofp-tcp-listen-port', default=None,
               help='openflow tcp listen port '
                    '(default: %d)' % ofproto_common.OFP_TCP_PORT),
    cfg.IntOpt('ofp-ssl-listen-port', default=None,
               help='openflow ssl listen port '
                    '(default: %d)' % ofproto_common.OFP_SSL_PORT),
    cfg.StrOpt('ctl-privkey', default=None, help='controller private key'),
    cfg.StrOpt('ctl-cert', default=None, help='controller certificate'),
    cfg.StrOpt('ca-certs', default=None, help='CA certificates'),
    cfg.StrOpt('ciphers', default=None, help='list of ciphers to enable'),
    cfg.ListOpt('ofp-switch-address-list', item_type=str, default=[],
                help='list of IP address and port pairs (default empty). '
                     'e.g., "127.0.0.1:6653,[::1]:6653"'),
    cfg.IntOpt('ofp-switch-connect-interval',
               default=DEFAULT_OFP_SW_CON_INTERVAL,
               help='interval in seconds to connect to switches '
                    '(default %d)' % DEFAULT_OFP_SW_CON_INTERVAL),
])
CONF.register_opts([
    cfg.FloatOpt('socket-timeout',
                 default=5.0,
                 help='Time, in seconds, to await completion of socket operations.'),
    cfg.FloatOpt('echo-request-interval',
                 default=15.0,
                 help='Time, in seconds, between sending echo requests to a datapath.'),
    cfg.IntOpt('maximum-unreplied-echo-requests',
               default=0,
               min=0,
               help='Maximum number of unreplied echo requests before datapath is disconnected.')
])


def _split_addr(addr):
    """
    Splits a str of IP address and port pair into (host, port).

    Example::

        >>> _split_addr('127.0.0.1:6653')
        ('127.0.0.1', 6653)
        >>> _split_addr('[::1]:6653')
        ('::1', 6653)

    Raises ValueError if invalid format.

    :param addr: A pair of IP address and port.
    :return: IP address and port
    """
    e = ValueError('Invalid IP address and port pair: "%s"' % addr)
    pair = addr.rsplit(':', 1)
    if len(pair) != 2:
        raise e

    addr, port = pair
    if addr.startswith('[') and addr.endswith(']'):
        addr = addr.lstrip('[').rstrip(']')
        if not ip.valid_ipv6(addr):
            raise e
    elif not ip.valid_ipv4(addr):
        raise e

    return addr, int(port, 0)


class OpenFlowController(object):
    def __init__(self):
        super(OpenFlowController, self).__init__()
        print("Starting openflow controller", CONF.ofp_tcp_listen_port)
        #if not CONF.ofp_tcp_listen_port and not CONF.ofp_ssl_listen_port:
        #    self.ofp_tcp_listen_port = ofproto_common.OFP_TCP_PORT
        #    self.ofp_ssl_listen_port = ofproto_common.OFP_SSL_PORT
        ##    # For the backward compatibility, we spawn a server loop
        ##    # listening on the old OpenFlow listen port 6633.
        #    print("Spawning init\n")
        #    hub.spawn(self.server_loop,
        #              ofproto_common.OFP_TCP_PORT_OLD,
        #              ofproto_common.OFP_SSL_PORT_OLD)
        #else:
        self.ofp_udp_listen_port = 6653      
        self.address = ()
        self.ofp_tcp_listen_port = CONF.ofp_tcp_listen_port
        self.ofp_ssl_listen_port = CONF.ofp_ssl_listen_port

        # Example:
        # self._clients = {
        #     ('127.0.0.1', 6653): <instance of StreamClient>,
        # }
        self._clients = {}

    # entry point
    def __call__(self):
        # LOG.debug('call')
        for address in CONF.ofp_switch_address_list:
            addr = tuple(_split_addr(address))
            self.spawn_client_loop(addr)

        self.server_loop()

    def spawn_client_loop(self, addr, interval=None):
        interval = interval or CONF.ofp_switch_connect_interval
        client = hub.StreamClient(addr)
        hub.spawn(client.connect_loop, datapath_connection_factory, interval)
        self._clients[addr] = client

    def stop_client_loop(self, addr):
        client = self._clients.get(addr, None)
        if client is not None:
            client.stop()

    def server_loop(self):

        UDPServerSocket = socket.socket(family=socket.AF_INET, type=socket.SOCK_DGRAM)
        UDPServerSocket.bind(("0.0.0.0", self.ofp_udp_listen_port))
        print("Port is ", self.ofp_udp_listen_port)
        print("New Ryu server up and listening")
        while True:
            bytesAddressPair = UDPServerSocket.recvfrom(4096)
            message = bytesAddressPair[0]
            address = bytesAddressPair[1]
            clientMsg = "Message from Client:{}".format(message)
            clientIP  = "Client IP Address:{}".format(address)
            #print(clientMsg)
            #print(clientIP)
            datapath_connection_factory_udp(UDPServerSocket, address, message);
            self.address = address

            #server = StreamServer((CONF.ofp_listen_host,
            #                       ofp_tcp_listen_port),
            #                      datapath_connection_factory)

        # LOG.debug('loop')
        #server.serve_forever()


def _deactivate(method):
    def deactivate(self):
        try:
            method(self)
        finally:
            try:
                self.socket.close()
            except IOError:
                pass

    return deactivate

#udp_socket = 0


class Datapath(ofproto_protocol.ProtocolDesc):
    """
    A class to describe an OpenFlow switch connected to this controller.

    An instance has the following attributes.

    .. tabularcolumns:: |l|L|

    ==================================== ======================================
    Attribute                            Description
    ==================================== ======================================
    id                                   64-bit OpenFlow Datapath ID.
                                         Only available for
                                         ryu.controller.handler.MAIN_DISPATCHER
                                         phase.
    ofproto                              A module which exports OpenFlow
                                         definitions, mainly constants appeared
                                         in the specification, for the
                                         negotiated OpenFlow version.  For
                                         example, ryu.ofproto.ofproto_v1_0 for
                                         OpenFlow 1.0.
    ofproto_parser                       A module which exports OpenFlow wire
                                         message encoder and decoder for the
                                         negotiated OpenFlow version.
                                         For example,
                                         ryu.ofproto.ofproto_v1_0_parser
                                         for OpenFlow 1.0.
    ofproto_parser.OFPxxxx(datapath,...) A callable to prepare an OpenFlow
                                         message for the given switch.  It can
                                         be sent with Datapath.send_msg later.
                                         xxxx is a name of the message.  For
                                         example OFPFlowMod for flow-mod
                                         message.  Arguemnts depend on the
                                         message.
    set_xid(self, msg)                   Generate an OpenFlow XID and put it
                                         in msg.xid.
    send_msg(self, msg)                  Queue an OpenFlow message to send to
                                         the corresponding switch.  If msg.xid
                                         is None, set_xid is automatically
                                         called on the message before queueing.
    send_packet_out                      deprecated
    send_flow_mod                        deprecated
    send_flow_del                        deprecated
    send_delete_all_flows                deprecated
    send_barrier                         Queue an OpenFlow barrier message to
                                         send to the switch.
    send_nxt_set_flow_format             deprecated
    is_reserved_port                     deprecated
    ==================================== ======================================
    """

    def __init__(self, socket, address):
        super(Datapath, self).__init__()

        self.socket = socket
        #self.socket.setsockopt(IPPROTO_TCP, TCP_NODELAY, 1)
        self.socket.settimeout(CONF.socket_timeout)
        self.address = address
        self.is_active = True

        # The limit is arbitrary. We need to limit queue size to
        # prevent it from eating memory up.
        self.send_q = hub.Queue(16)
        self._send_q_sem = hub.BoundedSemaphore(self.send_q.maxsize)

        self.echo_request_interval = CONF.echo_request_interval
        self.max_unreplied_echo_requests = CONF.maximum_unreplied_echo_requests
        self.unreplied_echo_requests = []

        self.xid = random.randint(0, self.ofproto.MAX_XID)
        self.id = None  # datapath_id is unknown yet
        self._ports = None
        self.flow_format = ofproto_v1_0.NXFF_OPENFLOW10
        self.ofp_brick = ryu.base.app_manager.lookup_service_brick('ofp_event')
        self.state = None  # for pylint
        self.set_state(HANDSHAKE_DISPATCHER)
        self.mac_to_port = {}

    def _close_write(self):
        # Note: Close only further sends in order to wait for the switch to
        # disconnect this connection.
        try:
            self.socket.shutdown(SHUT_WR)
        except (EOFError, IOError):
            pass

    def close(self):
        self.set_state(DEAD_DISPATCHER)
        self._close_write()

    def set_state(self, state):
        if self.state == state:
            return
        self.state = state
        ev = ofp_event.EventOFPStateChange(self)
        ev.state = state
        self.ofp_brick.send_event_to_observers(ev, state)

    # Low level socket handling layer
    #@_deactivate
    def _recv_loop(self, data):
        buf = bytearray()
        count = 0
        min_read_len = remaining_read_len = ofproto_common.OFP_HEADER_SIZE

        read_len = min_read_len
        if remaining_read_len > min_read_len:
            read_len = remaining_read_len
        ret = data
        buf = data
        buf_len = len(buf)
        while buf_len >= min_read_len:
            (version, msg_type, msg_len, xid) = ofproto_parser.header(buf)
            print("type msg_type is", msg_type)
            if msg_len < min_read_len:
                # Someone isn't playing nicely; log it, and try something sane.
                LOG.debug("Message with invalid length %s received from switch at address %s",
                          msg_len, self.address)
                msg_len = min_read_len
            if buf_len < msg_len:
                print("Buf len is less than msg len")
                remaining_read_len = (msg_len - buf_len)
                break

            msg = ofproto_parser.msg(
                self, version, msg_type, msg_len, xid, buf[:msg_len])
            # LOG.debug('queue msg %s cls %s', msg, msg.__class__)
            if msg:
                if(msg_type == 0): #Hello and set config
                    hello = self.ofproto_parser.OFPHello(self)
                    self.send_msg(hello)
                    
                    feature_request= self.ofproto_parser.OFPFeaturesRequest(self)
                    self.send_msg(feature_request)
                    
                    
                    
                    set_config = self.ofproto_parser.OFPSetConfig(self)
                    self.send_msg(set_config)
                elif msg_type == 6: #Feature request/reply
                    port_desc = self.ofproto_parser.OFPPortDescStatsRequest(self, 0)
                    self.send_msg(port_desc)

                elif msg_type == 2: #echo request/reply
                    echo_reply = self.ofproto_parser.OFPEchoReply(self)
                    echo_reply.xid = xid
                    echo_reply.data = data
                    self.send_msg(echo_reply)

                elif msg_type == 10: #OFP_PACKET_IN
                    ofproto = self.ofproto #have to check
                    parser = self.ofproto_parser
                    in_port = msg.match['in_port']
                    pkt = packet.Packet(data)
                    eth = pkt.get_protocols(ethernet.ethernet)[0]
                    if eth.ethertype == ether_types.ETH_TYPE_LLDP:
                        # ignore lldp packet
                        return
                    dst = eth.dst
                    src = eth.src

                    #print("type ", self.id)
                    #dpid = format(self.id, "d").zfill(16)
                    dpid = self.id
                    self.mac_to_port.setdefault(dpid, {})
                    print("packet in %s %s %s %s", dpid, src, dst, in_port)
                    #self.logger.info("packet in %s %s %s %s", dpid, src, dst, in_port)
                    self.mac_to_port[dpid][src] = in_port
                    if dst in self.mac_to_port[dpid]:
                        out_port = self.mac_to_port[dpid][dst]
                    else:
                        out_port = ofproto.OFPP_FLOOD

                    actions = [parser.OFPActionOutput(out_port)]
                    # install a flow to avoid packet_in next time
                    if out_port != ofproto.OFPP_FLOOD:
                        match = parser.OFPMatch(in_port=in_port, eth_dst=dst, eth_src=src)
                        if msg.buffer_id != ofproto.OFP_NO_BUFFER:
                            self.add_flow(datapath, 1, match, actions, msg.buffer_id)
                            return
                        else:
                            self.add_flow(datapath, 1, match, actions)

                    data = None
                    if msg.buffer_id == ofproto.OFP_NO_BUFFER:
                        data = data

                    match = parser.OFPMatch(in_port=in_port)
                    out = parser.OFPPacketOut(self, 0xffffffff,
                                  match=match, actions=actions, data=data)
                    print("sending packet in message out")
                    self.send_msg(out)



                    #Kumar


            buf = buf[msg_len:]
            buf_len = len(buf)
            remaining_read_len = min_read_len

            # We need to schedule other greenlets. Otherwise, ryu
            # can't accept new switches or handle the existing
            # switches. The limit is arbitrary. We need the better
            # approach in the future.
            count += 1
            if count > 2048:
                count = 0
                hub.sleep(0)

    def _send_loop(self):
        try:
            while self.state != DEAD_DISPATCHER:
                buf, close_socket = self.send_q.get()
                self._send_q_sem.release()
                
                self.socket.sendall(buf)
                if close_socket:
                    break
        except SocketTimeout:
            LOG.debug("Socket timed out while sending data to switch at address %s",
                      self.address)
        except IOError as ioe:
            # Convert ioe.errno to a string, just in case it was somehow set to None.
            errno = "%s" % ioe.errno
            LOG.debug("Socket error while sending data to switch at address %s: [%s] %s",
                      self.address, errno, ioe.strerror)
        finally:
            q = self.send_q
            # First, clear self.send_q to prevent new references.
            self.send_q = None
            # Now, drain the send_q, releasing the associated semaphore for each entry.
            # This should release all threads waiting to acquire the semaphore.
            try:
                while q.get(block=False):
                    self._send_q_sem.release()
            except hub.QueueEmpty:
                pass
            # Finally, disallow further sends.
            self._close_write()

    def send(self, buf, close_socket=False):
        self.socket.sendto(buf, self.address)

    def set_xid(self, msg):
        self.xid += 1
        self.xid &= self.ofproto.MAX_XID
        msg.set_xid(self.xid)
        return self.xid

    def send_msg(self, msg, close_socket=False):
        assert isinstance(msg, self.ofproto_parser.MsgBase)
        if msg.xid is None:
            self.set_xid(msg)
        msg.serialize()
        # LOG.debug('send_msg %s', msg)
        return self.send(msg.buf, close_socket=close_socket)

    def _echo_request_loop(self):
        if not self.max_unreplied_echo_requests:
            return
        while (self.send_q and
               (len(self.unreplied_echo_requests) <= self.max_unreplied_echo_requests)):
            echo_req = self.ofproto_parser.OFPEchoRequest(self)
            self.unreplied_echo_requests.append(self.set_xid(echo_req))
            self.send_msg(echo_req)
            hub.sleep(self.echo_request_interval)
        self.close()

    def acknowledge_echo_reply(self, xid):
        try:
            self.unreplied_echo_requests.remove(xid)
        except ValueError:
            pass

    def kill(thread):
        thread.kill()

    def serve(self, data):
        print("Spawning serve too")
        self._recv_loop(data)
        self.is_active = False

    #
    # Utility methods for convenience
    #
    def send_packet_out(self, buffer_id=0xffffffff, in_port=None,
                        actions=None, data=None):
        if in_port is None:
            in_port = self.ofproto.OFPP_NONE
        packet_out = self.ofproto_parser.OFPPacketOut(
            self, buffer_id, in_port, actions, data)
        self.send_msg(packet_out)

    def send_flow_mod(self, rule, cookie, command, idle_timeout, hard_timeout,
                      priority=None, buffer_id=0xffffffff,
                      out_port=None, flags=0, actions=None):
        if priority is None:
            priority = self.ofproto.OFP_DEFAULT_PRIORITY
        if out_port is None:
            out_port = self.ofproto.OFPP_NONE
        flow_format = rule.flow_format()
        assert (flow_format == ofproto_v1_0.NXFF_OPENFLOW10 or
                flow_format == ofproto_v1_0.NXFF_NXM)
        if self.flow_format < flow_format:
            self.send_nxt_set_flow_format(flow_format)
        if flow_format == ofproto_v1_0.NXFF_OPENFLOW10:
            match_tuple = rule.match_tuple()
            match = self.ofproto_parser.OFPMatch(*match_tuple)
            flow_mod = self.ofproto_parser.OFPFlowMod(
                self, match, cookie, command, idle_timeout, hard_timeout,
                priority, buffer_id, out_port, flags, actions)
        else:
            flow_mod = self.ofproto_parser.NXTFlowMod(
                self, cookie, command, idle_timeout, hard_timeout,
                priority, buffer_id, out_port, flags, rule, actions)
        self.send_msg(flow_mod)

    def send_flow_del(self, rule, cookie, out_port=None):
        self.send_flow_mod(rule=rule, cookie=cookie,
                           command=self.ofproto.OFPFC_DELETE,
                           idle_timeout=0, hard_timeout=0, priority=0,
                           out_port=out_port)

    def send_delete_all_flows(self):
        rule = nx_match.ClsRule()
        self.send_flow_mod(
            rule=rule, cookie=0, command=self.ofproto.OFPFC_DELETE,
            idle_timeout=0, hard_timeout=0, priority=0, buffer_id=0,
            out_port=self.ofproto.OFPP_NONE, flags=0, actions=None)

    def send_barrier(self):
        barrier_request = self.ofproto_parser.OFPBarrierRequest(self)
        return self.send_msg(barrier_request)

    def send_nxt_set_flow_format(self, flow_format):
        assert (flow_format == ofproto_v1_0.NXFF_OPENFLOW10 or
                flow_format == ofproto_v1_0.NXFF_NXM)
        if self.flow_format == flow_format:
            # Nothing to do
            return
        self.flow_format = flow_format
        set_format = self.ofproto_parser.NXTSetFlowFormat(self, flow_format)
        # FIXME: If NXT_SET_FLOW_FORMAT or NXFF_NXM is not supported by
        # the switch then an error message will be received. It may be
        # handled by setting self.flow_format to
        # ofproto_v1_0.NXFF_OPENFLOW10 but currently isn't.
        self.send_msg(set_format)
        self.send_barrier()

    def is_reserved_port(self, port_no):
        return port_no > self.ofproto.OFPP_MAX

def datapath_connection_factory_udp(socket, address, data):
    LOG.debug('connected socket:%s address:%s', socket, address)
    datapath = Datapath(socket, address)
    datapath.serve(data)
    
def datapath_connection_factory(socket, address, data):
    LOG.debug('connected socket:%s address:%s', socket, address)
    with contextlib.closing(Datapath(socket, address)) as datapath:
        try:
            datapath.serve(data)
        except:
            # Something went wrong.
            # Especially malicious switch can send malformed packet,
            # the parser raise exception.
            # Can we do anything more graceful?
            if datapath.id is None:
                dpid_str = "%s" % datapath.id
            else:
                dpid_str = dpid_to_str(datapath.id)
            LOG.error("Error in the datapath %s from %s", dpid_str, address)
            raise
