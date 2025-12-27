import os
import subprocess
import requests
import socket
import time
from prettytable import PrettyTable

# subprocess settings
EXECUTABLE = "proxyServer"
C_FILE = "*.c"
H_FILES = "*.h"
PORT = 3000

# timeout settings
WAIT_TO_INIT = 3
SOCKET_TIMEOUT = 10
DEADLOCK_TIMEOUT = 20


class LocalHostClient:
    """
    TCP client that connects to localhost
    """

    def __init__(self, port, timeout=10):
        self._s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            self._s.connect(('localhost', port))
            self._s.settimeout(timeout)
        except ConnectionError as e:
            self._s.close()
            raise e

    def recvall(self) -> bytes:
        rbytes = b""
        while True:

            try:
                data = self._s.recv(1024)
            except BrokenPipeError as e:
                self._s.close()
                raise e

            if not data:
                break
            rbytes += data
        return rbytes

    def sendall(self, data: bytes):
        try:
            self._s.sendall(data)
        except BrokenPipeError as e:
            self._s.close()
            raise e

    def die(self):
        self._s.close()


def check_errors(file_name: str):
    summery = "ERROR SUMMARY: "
    with open(file_name, "r") as valgrind_log:
        log_output = valgrind_log.read()

    summmey_idx = log_output.find(summery)

    if summmey_idx == -1:
        raise IndexError

    errs_loc = log_output[summmey_idx + len(summery):]
    num_of_errs = errs_loc.split(' ', 1)[0]

    if int(num_of_errs) != 0:
        return True

    return False


def check_leaks(file_name: str):
    leaks = True
    with open(file_name, 'r') as valgrind_log:
        val_lines = valgrind_log.readlines()
        for line in val_lines:
            if "no leaks are possible" in line:
                leaks = False

        if leaks:
            print("[-] valgrind found memory leaks, check your code for allocation/freeing errors.")
            return True
    return False


def valgrind():
    print("[!] Valgrind Test")
    subprocess_args = ["valgrind", "--leak-check=full", "--tool=memcheck", "--show-leak-kinds=all",
                       "--track-origins=yes", "--verbose", "--error-exitcode=1", "-v",
                       "--log-file=valgrind-out.txt", f"./{EXECUTABLE}", str(PORT), "3", "2", "filter.txt"]
    proc = subprocess.Popen(subprocess_args, stderr=subprocess.PIPE)
    time.sleep(WAIT_TO_INIT)

    try:
        with open("stdout_valgrind.txt", "wb") as fp:
            cli1 = LocalHostClient(PORT, timeout=SOCKET_TIMEOUT)
            cli2 = LocalHostClient(PORT, timeout=SOCKET_TIMEOUT)

            cli1.sendall(b"GET / HTTP/1.1\r\nHost: octopress.org\r\nConnection: close\r\n\r\n")
            cli2.sendall(b"GET / HTTP/1.1\r\nHost: http.badssl.com\r\n\r\n")

            res1 = cli1.recvall()
            res2 = cli2.recvall()

            fp.write(res1 + b"\n" + res2)
            proc.communicate(timeout=5)

    except (
            subprocess.TimeoutExpired, socket.timeout,
            ConnectionError, BrokenPipeError
    ) as e:
        if isinstance(e, ConnectionRefusedError):
            err = proc.communicate(timeout=3)
            if "in use" in str(e):
                raise ConnectionRefusedError(e)

        proc.terminate()
        return False

    cli1.die()
    cli2.die()

    if check_leaks("valgrind-out.txt") is True:
        return False

    if check_errors("valgrind-out.txt") is True:
        return False

    res1 = res1.lower()
    res2 = res2.lower()

    if b"200 OK".lower() not in res1:
        return False
    if b"403 Forbidden".lower() not in res2:
        return False

    if proc.returncode > 127:
        return False

    return True


def deadlock():
    print("[!] Deadlock Test")
    subprocess_args = [f"./{EXECUTABLE}", str(PORT), "4", "10", "filter.txt"]
    proc = subprocess.Popen(subprocess_args, stderr=subprocess.PIPE)
    time.sleep(WAIT_TO_INIT)

    clients, res = [], []
    reqs = [
        b"GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n",
        b"GET / HTTP/1.1\r\nHost: pdf995.com\r\nConnection: close\r\n\r\n",
        b"GET / HTTP/1.1\r\nHost: clearshinyshininglight.neverssl.com\r\nConnection: close\r\n\r\n",
        b"GET / HTTP/1.1\r\nHost: octopress.org\r\nConnection: close\r\n\r\n",
        b"GET / HTTP/1.1\r\nHost: info.cern.ch\r\nConnection: close\r\n\r\n",
        b"GET /samples/widgets.pdf HTTP/1.1\r\nHost: pdf995.com\r\nConnection: close\r\n\r\n",
        b"GET / HTTP/5.6\r\nHost: example.com\r\n\r\n",  # bad request
        b"UPDATE / HTTP/1.1\r\nHost: example.com\r\n\r\n",  # not supported
        b"GET /index HTTP/1.1\r\nHost: myunknownhost.com\r\n\r\n",  # not found
        b"GET / HTTP/1.1\r\nHost: http.badssl.com\r\n\r\n",  # forbidden
    ]

    with open("stdout_deadlock.txt", "wb") as fp:
        try:
            for i in range(5):
                clients.append(LocalHostClient(PORT, timeout=DEADLOCK_TIMEOUT))

            for i in range(5):
                clients[i].sendall(reqs[i])

            for i in range(5):
                res.append(clients[i].recvall())

            for i in range(5):
                clients.append(LocalHostClient(PORT, timeout=DEADLOCK_TIMEOUT))
                clients[i + 5].sendall(reqs[i + 5])
                res.append(clients[i + 5].recvall())

        except (socket.timeout, ConnectionError, BrokenPipeError) as e:
            print(e)
            if isinstance(e, ConnectionRefusedError):
                _, err = proc.communicate(timeout=3)
                if "in use" in str(err):
                    raise ConnectionRefusedError(e)

            for c in clients:
                c.die()
            proc.terminate()
            return False

        fp.write(b"\n".join(res))

    res = [r.lower() for r in res]

    for c in clients:
        c.die()

    if sum(r.count(b"200 OK".lower()) for r in res) != 6:
        return False
    if b"400 Bad Request".lower() not in res[6]:
        return False
    if b"501 Not supported".lower() not in res[7]:
        return False
    if b"404 Not Found".lower() not in res[8]:
        return False
    if b"403 Forbidden".lower() not in res[9]:
        return False

    try:
        proc.communicate(timeout=3)
    except subprocess.TimeoutExpired as e:
        print(e)
        proc.terminate()
        return False

    if proc.returncode > 127:
        return False

    return True


def keep_alive():
    """
    Send a request with Connection: keep-alive header
    """
    print("[!] Keep Alive Test")
    subprocess_args = [f"./{EXECUTABLE}", str(PORT), "1", "1", "filter.txt"]
    proc = subprocess.Popen(subprocess_args, stderr=subprocess.PIPE)
    time.sleep(WAIT_TO_INIT)

    try:
        with open("stdout_keep_alive.txt", "wb") as fp:
            cli = LocalHostClient(PORT, timeout=10)
            cli.sendall(b"GET / HTTP/1.1\r\nHost: example.com\r\nConnection: keep-alive\r\n\r\n")
            res = cli.recvall()
            proc.communicate(timeout=SOCKET_TIMEOUT)
            fp.write(res)

    except (
            subprocess.TimeoutExpired, socket.timeout,
            ConnectionError, BrokenPipeError
    ) as e:
        if isinstance(e, ConnectionRefusedError):
            _, err = proc.communicate(timeout=3)
            if "in use" in str(err):
                raise ConnectionRefusedError(e)
        proc.terminate()
        return False

    cli.die()
    if b"200 OK" not in res:
        return False
    if proc.returncode > 127:
        return False

    return True


def unsigned_char_support():
    """
    Request for an image and check whether all bytes have been received
    """
    print("[!] Unsigned Char Support Test")
    subprocess_args = [f"./{EXECUTABLE}", str(PORT), "1", "1", "filter.txt"]
    proc = subprocess.Popen(subprocess_args, stderr=subprocess.PIPE)
    time.sleep(WAIT_TO_INIT)

    try:
        with open("stdout_unsigned_char_support.txt", "wb") as fp:
            cli = LocalHostClient(PORT, timeout=SOCKET_TIMEOUT)
            req = b"GET /images2015/wa_banner_excellence1024-plain.jpg HTTP/1.1\r\n" \
                  b"Host: webaward.org\r\n" \
                  b"Connection: close\r\n\r\n"

            cli.sendall(req)
            res = cli.recvall()
            proc.communicate(timeout=3)
            fp.write(res)

            req = requests.get("http://webaward.org/images2015/wa_banner_excellence1024-plain.jpg")
            if req.status_code != 200:
                raise ValueError("invalid request status code")

    except (
            subprocess.TimeoutExpired, socket.timeout,
            ConnectionError, BrokenPipeError
    ) as e:
        print(e)
        if isinstance(e, ConnectionRefusedError):
            _, err = proc.communicate(timeout=3)
            if "in use" in str(err):
                raise ConnectionRefusedError(e)
        proc.terminate()
        return False

    cli.die()
    if req.content not in res:
        return False
    if proc.returncode > 127:
        return False

    return True


def is_read_within_loop():
    """
    Slice request and send it in parts
    """
    print("[!] Is Read Within Loop Test")
    subprocess_args = [f"./{EXECUTABLE}", str(PORT), "1", "1", "filter.txt"]
    proc = subprocess.Popen(subprocess_args, stderr=subprocess.PIPE)
    time.sleep(WAIT_TO_INIT)

    try:
        with open("stdout_read_within_loop.txt", "wb") as fp:
            cli = LocalHostClient(PORT, timeout=10)
            req_slices = [
                b"GET ", b"/ ", b"HTTP/1.1\r\n",
                b"Host: ", b"example.com\r\n",
                b"Connection: ", b"close\r\n\r\n"
            ]

            for rslice in req_slices:
                time.sleep(0.5)
                cli.sendall(rslice)

            res = cli.recvall()
            proc.communicate(timeout=3)
            fp.write(res)

    except (
            subprocess.TimeoutExpired, socket.timeout,
            ConnectionError, BrokenPipeError
    ) as e:
        if isinstance(e, ConnectionRefusedError):
            _, err = proc.communicate(timeout=3)
            if "in use" in str(err):
                raise ConnectionRefusedError(e)
        proc.terminate()
        return False

    cli.die()
    if b"200 OK" not in res:
        return False
    if proc.returncode > 127:
        return False

    return True


def validate_response_error(response: bytes, code: int) -> bool:
    response = response.lower()

    if code == 400:
        err = b"400 Bad Request"
        description = "Bad Request."
    elif code == 403:
        err = b"403 Forbidden"
        description = "Access denied."
    elif code == 404:
        err = b"404 Not Found"
        description = "File not found."
    elif code == 500:
        err = b"500 Internal Server Error"
        description = "Some server side error."
    elif code == 501:
        err = b"501 Not supported"
        description = "Method is not supported."
    else:
        raise ValueError("invalid response code")

    body1 = f"<HTML><HEAD><TITLE>{err.decode()}</TITLE></HEAD>\r\n" \
            f"<BODY><H4>{err.decode()}</H4>\r\n" \
            f"{description}\r\n" \
            f"</BODY></HTML>\r\n".encode()

    body2 = f"<HTML><HEAD><TITLE>{err.decode()}</TITLE></HEAD>\n" \
            f"<BODY><H4>{err.decode()}</H4>\n" \
            f"{description}\n" \
            f"</BODY></HTML>\n".encode()

    body3 = f"<HTML><HEAD><TITLE>{err.decode()}</TITLE></HEAD>\r\n" \
            f"<BODY><H4>{err.decode()}</H4>\r\n" \
            f"{description}\r\n" \
            f"</BODY></HTML>".encode()

    body4 = f"<HTML><HEAD><TITLE>{err.decode()}</TITLE></HEAD>\n" \
            f"<BODY><H4>{err.decode()}</H4>\n" \
            f"{description}\n" \
            f"</BODY></HTML>".encode()

    body5 = f"<HTML><HEAD><TITLE>{err.decode()}</TITLE></HEAD>" \
            f"<BODY><H4>{err.decode()}</H4>" \
            f"{description}" \
            f"</BODY></HTML>".encode()

    content_length1 = f"Content-Length: {len(body1)}\r\n".encode()
    content_length2 = f"Content-Length: {len(body2)}\r\n".encode()
    content_length3 = f"Content-Length: {len(body3)}\r\n".encode()
    content_length4 = f"Content-Length: {len(body4)}\r\n".encode()
    content_length5 = f"Content-Length: {len(body5)}\r\n".encode()

    if (b"HTTP/1.1 " + err + b"\r\n").lower() not in response:
        return False
    if b"Server: webserver/1.0\r\n".lower() not in response:
        return False
    if b"Date: ".lower() not in response:
        return False
    if b"Content-Type: text/html\r\n".lower() not in response:
        return False
    if b"Connection: close\r\n".lower() not in response:
        return False

    if content_length1.lower() in response and body1.lower() in response:
        pass
    elif content_length2.lower() in response and body2.lower() in response:
        pass
    elif content_length3.lower() in response and body3.lower() in response:
        pass
    elif content_length4.lower() in response and body4.lower() in response:
        pass
    elif content_length5.lower() in response and body5.lower() in response:
        pass
    else:
        return False

    return True


def invalid_request(stdout_name: str, req: bytes, code: int) -> bool:
    subprocess_args = [f"./{EXECUTABLE}", str(PORT), "1", "1", "filter.txt"]
    proc = subprocess.Popen(subprocess_args, stderr=subprocess.PIPE)
    time.sleep(WAIT_TO_INIT)

    try:
        with open(stdout_name, "wb") as fp:
            cli = LocalHostClient(PORT, timeout=10)
            cli.sendall(req)
            res = cli.recvall()

            proc.communicate(timeout=3)
            fp.write(res)

    except (
            subprocess.TimeoutExpired, socket.timeout,
            ConnectionError, BrokenPipeError
    ) as e:
        if isinstance(e, ConnectionRefusedError):
            _, err = proc.communicate(timeout=3)
            if "in use" in str(err):
                raise ConnectionRefusedError(e)
        proc.terminate()
        return False

    cli.die()
    if not validate_response_error(res, code):
        return False

    if proc.returncode > 127:
        return False

    return True


def forbidden():
    """
    HTTP request with forbidden ip
    """
    print("[!] Forbidden Test")
    return invalid_request(
        "stdout_forbidden.txt",
        req=b"GET / HTTP/1.1\r\nHost: http.badssl.com\r\n\r\n",
        code=403
    )


def not_found():
    """
    HTTP request with invalid host
    """
    print("[!] Not Found Test")
    return invalid_request(
        "stdout_not_found.txt",
        req=b"GET /index HTTP/1.1\r\nHost: myunknownhost.com\r\n\r\n",
        code=404
    )


def not_supported():
    """
    HTTP request with UPDATE method
    """
    print("[!] Not Supported Test")
    return invalid_request(
        "stdout_not_supported.txt",
        req=b"UPDATE / HTTP/1.1\r\nHost: example.com\r\n\r\n",
        code=501
    )


def bad_request():
    """
    HTTP request with bad HTTP version
    """
    print("[!] Bad Request Test")
    return invalid_request(
        "stdout_bad_request.txt",
        req=b"GET / HTTP/5.6\r\nHost: example.com\r\n\r\n",
        code=400
    )


def usage(args: str, v: int):
    """
    1) Usage Test: ProxyServer
    2) Usage Test ProxyServer 3000 2 -1
    """
    print(f"[!] Usage{1} Test")
    try:
        with open(f"stdout_test_usage_{v}.txt", "w") as fp:
            proc = subprocess.run(
                [f"./{EXECUTABLE}"] + args.split(" "),
                stdout=fp,
                stderr=fp,
                text=True,
                timeout=5
            )

    except subprocess.TimeoutExpired as e:
        print(e)
        return False

    try:
        with open(f"stdout_test_usage_{v}.txt") as fp:
            res = fp.read().lower().rstrip()

    except UnicodeDecodeError as e:
        print(e)
        return False

    usage1 = "Usage: proxyServer <port> <pool-size> <max-number-of-request> <filter>".lower()
    if res != usage1 and res != "./" + usage1:
        return False

    return True


def setup():
    if os.path.isfile(EXECUTABLE):
        os.remove(EXECUTABLE)

    with open("stdout_compilation.txt", 'w') as out_file:
        c = subprocess.run(
            f'gcc -Wall {C_FILE} {H_FILES} -o {EXECUTABLE} -lpthread',
            stderr=out_file,
            stdout=out_file,
            shell=True,
        )

    with open("stdout_compilation.txt") as out_file:
        res = out_file.read()
        return_val = None
        if bytes(res, 'utf-8') == b'':
            print("Ex. compiled successfully.")
            return_val = "Compiled"

        if "warning: " in res:
            print("Warnings during compilation")
            return_val = "Warnings"

        if "error: " in res:
            print("\nSomething didn't go right when compiling your C source "
                  "please check stdout_compilation.txt\n")
            return_val = "Error"
        return return_val


if __name__ == "__main__":
    compilation_status = setup()
    if compilation_status == "Error":
        exit(0)

    t_usage1 = usage("", v=1)
    PORT = PORT + 1
    t_usage2 = usage("3000 1 3", v=2)
    PORT = PORT + 1
    t_bad_request = bad_request()
    PORT = PORT + 1
    t_not_found = not_found()
    PORT = PORT + 1
    t_not_supported = not_supported()
    PORT = PORT + 1
    t_forbidden = forbidden()
    PORT = PORT + 1
    t_is_read_within_loop = is_read_within_loop()
    PORT = PORT + 1
    t_unsigned_char = unsigned_char_support()
    PORT = PORT + 1
    t_keep_alive = keep_alive()
    PORT = PORT + 1
    t_deadlock = deadlock()
    PORT = PORT + 1
    t_valgrind = valgrind()

    t = PrettyTable(["Test", "Result"])
    t.align['Test'] = 'l'
    t.add_row(["Usage1", t_usage1])
    t.add_row(["Usage2", t_usage2])
    t.add_row(["Bad Request", t_bad_request])
    t.add_row(["Not Found", t_not_found])
    t.add_row(["Not Supported", t_not_supported])
    t.add_row(["Forbidden", t_forbidden])
    t.add_row(["Is Read Within Loop", t_is_read_within_loop])
    t.add_row(["Is Unsigned Char Supported", t_unsigned_char])
    t.add_row(["Keep Alive", t_keep_alive])
    t.add_row(["Deadlock", t_deadlock])
    t.add_row(["Valgrind", t_valgrind])
    print(t)
