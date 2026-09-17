import socket

HOST = "0.0.0.0"  # listen on all interfaces
PORT = 8888


def main():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server_socket:
        server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server_socket.bind((HOST, PORT))
        server_socket.listen(1)
        print(f"Server is ready to receive message on port {PORT}")

        conn, addr = server_socket.accept()
        with conn:
            while True:
                data = conn.recv(1024)
                if not data:
                    break

                print(f"From Client: {data}")
                message = data.decode()

                if message == "bye":
                    break

                conn.sendall(f"XXX{message}".encode())

        print("Client disconnected")


if __name__ == "__main__":
    main()
