import socket

def open_udp_socket(ofp_udp_listen_port):
    UDPServerSocket = socket.socket(family=socket.AF_INET, type=socket.SOCK_DGRAM)
    UDPServerSocket.bind(("0.0.0.0", ofp_udp_listen_port))
    return UDPServerSocket

