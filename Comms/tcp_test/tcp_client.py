import socket

# For a local (-L) tunnel or same-network testing: "localhost"
# For a reverse (-R) tunnel where this script runs on the Ultra96
#   (the tunnel makes "localhost" on the Ultra96 side forward back to the laptop)
HOST = "localhost"
PORT = 8888


def main():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as client_socket:
        client_socket.connect((HOST, PORT))
        print(f"Connecting {HOST} : {PORT}")

        while True:
            message = input("Enter a message (enter 'bye' to disconnect): ")
            client_socket.sendall(message.encode())

            if message == "bye":
                break

            response = client_socket.recv(1024)
            print(f"from server: {response.decode()}")


if __name__ == "__main__":
    main()
