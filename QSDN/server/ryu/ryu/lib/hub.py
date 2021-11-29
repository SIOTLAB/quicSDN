# Copyright (C) 2013 Nippon Telegraph and Telephone Corporation.
# Copyright (C) 2013 YAMAMOTO Takashi <yamamoto at valinux co jp>
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

import logging
import os
from ryu.lib import ip

from ryu.ofproto import ofproto_common
from ryu.ofproto import ofproto_parser
from ryu.ofproto import ofproto_protocol
from ryu.ofproto import ofproto_v1_0
from ryu.ofproto import nx_match

from ryu.controller import ofp_event
from ryu.controller.handler import HANDSHAKE_DISPATCHER, DEAD_DISPATCHER

from ryu.lib.dpid import dpid_to_str



# We don't bother to use cfg.py because monkey patch needs to be
# called very early. Instead, we use an environment variable to
# select the type of hub.
HUB_TYPE = os.getenv('RYU_HUB_TYPE', 'eventlet')

LOG = logging.getLogger('ryu.lib.hub')

if HUB_TYPE == 'eventlet':
    import eventlet
    # HACK:
    # sleep() is the workaround for the following issue.
    # https://github.com/eventlet/eventlet/issues/401
    eventlet.sleep()
    import eventlet.event
    import eventlet.queue
    import eventlet.semaphore
    import eventlet.timeout
    import eventlet.wsgi
    from eventlet import websocket
    import greenlet
    import ssl
    import socket
    import traceback
    import sys

    getcurrent = eventlet.getcurrent
    patch = eventlet.monkey_patch
    sleep = eventlet.sleep
    listen = eventlet.listen
    connect = eventlet.connect

    def spawn(*args, **kwargs):
        raise_error = kwargs.pop('raise_error', False)

        def _launch(func, *args, **kwargs):
            # Mimic gevent's default raise_error=False behaviour
            # by not propagating an exception to the joiner.
            try:
                return func(*args, **kwargs)
            except TaskExit:
                pass
            except BaseException as e:
                if raise_error:
                    raise e
                # Log uncaught exception.
                # Note: this is an intentional divergence from gevent
                # behaviour; gevent silently ignores such exceptions.
                LOG.error('hub: uncaught exception: %s',
                          traceback.format_exc())

        return eventlet.spawn(_launch, *args, **kwargs)

    def spawn_after(seconds, *args, **kwargs):
        raise_error = kwargs.pop('raise_error', False)

        def _launch(func, *args, **kwargs):
            # Mimic gevent's default raise_error=False behaviour
            # by not propagating an exception to the joiner.
            try:
                return func(*args, **kwargs)
            except TaskExit:
                pass
            except BaseException as e:
                if raise_error:
                    raise e
                # Log uncaught exception.
                # Note: this is an intentional divergence from gevent
                # behaviour; gevent silently ignores such exceptions.
                LOG.error('hub: uncaught exception: %s',
                          traceback.format_exc())

        return eventlet.spawn_after(seconds, _launch, *args, **kwargs)

    def kill(thread):
        thread.kill()

    def joinall(threads):
        for t in threads:
            # This try-except is necessary when killing an inactive
            # greenthread.
            try:
                t.wait()
            except TaskExit:
                pass

    Queue = eventlet.queue.LightQueue
    QueueEmpty = eventlet.queue.Empty
    Semaphore = eventlet.semaphore.Semaphore
    BoundedSemaphore = eventlet.semaphore.BoundedSemaphore
    TaskExit = greenlet.GreenletExit

    class StreamServer(object):
        def __init__(self, listen_info, handle=None, backlog=None,
                     spawn='default', **ssl_args):
            assert backlog is None
            assert spawn == 'default'

        def process_data(self, data):
            buf =  bytearray()
            count = 0
            min_read_len = remaining_read_len = ofproto_common.OFP_HEADER_SIZE
            read_len = min_read_len
            if remaining_read_len > min_read_len:
                read_len = remaining_read_len

            ofp_brick = ryu.base.app_manager.lookup_service_brick('ofp_event')
            ret = data
            buf = data
            print(buf)
            buf_len = len(buf)
            while buf_len >= min_read_len:
                (version, msg_type, msg_len, xid) = ofproto_parser.header(buf)
                if msg_len < min_read_len:
                    LOG.debug("Message with invalid length %s received from switch at address %s", \
                            msg_len, self.address)
                    msg_len = min_read_len
                if buf_len < msg_len:
                    remaining_read_len = (msg_len - buf_len)
                    break
                msg = ofproto_parser.msg(self, version, msg_type, \
                                        msg_len, xid, buf[:msg_len])
                if msg:
                    ev = ofp_event.ofp_msg_to_ev(msg)
                    ofp_brick.send_event_to_observers(ev, self.state)

                    def dispatchers(x):
                        return x.callers[ev.__class__].dispatchers

                    handlers = [handler for handler in
                                self.ofp_brick.get_handlers(ev) if
                                self.state in dispatchers(handler)]

                    for handler in handlers:
                        handler(ev)

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

        def serve_forever(self):
            while True:
                UDPServerSocket = socket.socket(family=socket.AF_INET, type=socket.SOCK_DGRAM)
                UDPServerSocket.bind(("0.0.0.0", 6654))
                print("New Ryu server up and listening")
                bytesAddressPair = UDPServerSocket.recvfrom(4096)
                message = bytesAddressPair[0]
                address = bytesAddressPair[1]
                clientMsg = "Message from Client:{}".format(message)
                clientIP  = "Client IP Address:{}".format(address)
                print(clientMsg)
                print(clientIP)
                self.process_data(clientMsg)

    class StreamClient(object):
        def __init__(self, addr, timeout=None, **ssl_args):
            assert ip.valid_ipv4(addr[0]) or ip.valid_ipv6(addr[0])
            self.addr = addr
            self.timeout = timeout
            self.ssl_args = ssl_args
            self._is_active = True

        def connect(self):
            try:
                if self.timeout is not None:
                    client = socket.create_connection(self.addr,
                                                      timeout=self.timeout)
                else:
                    client = socket.create_connection(self.addr)
            except socket.error:
                return None

            if self.ssl_args:
                client = ssl.wrap_socket(client, **self.ssl_args)

            return client

        def connect_loop(self, handle, interval):
            while self._is_active:
                sock = self.connect()
                if sock:
                    handle(sock, self.addr)
                sleep(interval)

        def stop(self):
            self._is_active = False

    class LoggingWrapper(object):
        def write(self, message):
            LOG.info(message.rstrip('\n'))

    class WSGIServer(StreamServer):
        def serve_forever(self):
            self.logger = LoggingWrapper()
            eventlet.wsgi.server(self.server, self.handle, self.logger)

    WebSocketWSGI = websocket.WebSocketWSGI

    Timeout = eventlet.timeout.Timeout

    class Event(object):
        def __init__(self):
            self._ev = eventlet.event.Event()
            self._cond = False

        def _wait(self, timeout=None):
            while not self._cond:
                self._ev.wait()

        def _broadcast(self):
            self._ev.send()
            # Since eventlet Event doesn't allow multiple send() operations
            # on an event, re-create the underlying event.
            # Note: _ev.reset() is obsolete.
            self._ev = eventlet.event.Event()

        def is_set(self):
            return self._cond

        def set(self):
            self._cond = True
            self._broadcast()

        def clear(self):
            self._cond = False

        def wait(self, timeout=None):
            if timeout is None:
                self._wait()
            else:
                try:
                    with Timeout(timeout):
                        self._wait()
                except Timeout:
                    pass

            return self._cond
