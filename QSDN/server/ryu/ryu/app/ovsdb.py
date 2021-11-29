
from ryu.base import app_manager
from ryu.controller.handler import set_ev_cls
from ryu.services.protocols.ovsdb import api as ovsdb
from ryu.services.protocols.ovsdb import event as ovsdb_event


class MyApp(app_manager.RyuApp):
    @set_ev_cls(ovsdb_event.EventNewOVSDBConnection)
    def handle_new_ovsdb_connection(self, ev):
        system_id = ev.system_id
        address = ev.client.address
        self.logger.info(
            'New OVSDB connection from system-id=%s, address=%s',
            system_id, address)

        # Example: If device has bridge "s1", add port "s1-eth99"
        if ovsdb.bridge_exists(self, system_id, "s1"):
            self.create_port(system_id, "s1", "s1-eth99")

    def create_port(self, system_id, bridge_name, name):
        new_iface_uuid = uuid.uuid4()
        new_port_uuid = uuid.uuid4()

        bridge = ovsdb.row_by_name(self, system_id, bridge_name)

        def _create_port(tables, insert):
            iface = insert(tables['Interface'], new_iface_uuid)
            iface.name = name
            iface.type = 'internal'

            port = insert(tables['Port'], new_port_uuid)
            port.name = name
            port.interfaces = [iface]

            bridge.ports = bridge.ports + [port]

            return new_port_uuid, new_iface_uuid

        req = ovsdb_event.EventModifyRequest(system_id, _create_port)
        rep = self.send_request(req)

        if rep.status != 'success':
            self.logger.error('Error creating port %s on bridge %s: %s',
                              name, bridge, rep.status)
            return None

        return rep.insert_uuids[new_port_uuid]

