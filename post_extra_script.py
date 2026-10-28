Import("env")

def wait_for_port(port):
    print(f"Waiting for port {port}...")
    import serial
    while True:
        try:
            serial.Serial(port)
            return
        except:
            pass

def get_env_port(env, option):
    port = env.GetProjectOption(option)
    if port:
        print(f"Using {option} {port}...")

    return port

# see https://github.com/platformio/platformio-core/issues/3742#issuecomment-1003454439
def wait_for_monitor_port(source, target, env):
    # print(env.Dump())
    # "pio test" has no delay between upload & monitoring. Unfortuneatly, the teensy
    # is rebooting at that point and the port isn't available. This rasies an exception. 
    port = get_env_port(env, "monitor_port") or get_env_port(env, "upload_port") or get_env_port(env, "test_port")
    if port is None:
        from platformio.builder.tools.pioupload import AutodetectUploadPort
        try:
            AutodetectUploadPort(env)
            port = env.subst("$UPLOAD_PORT")
        except Exception as exc:
            # AutodetectUploadPort enumerates the boards of EVERY installed platform.
            # A malformed third-party platform (atmelsam has a board with no openocd
            # chipname) makes it raise, which would fail the whole upload even though
            # the upload itself already succeeded. Port autodetection is only needed to
            # pause briefly before opening the monitor, so a failure here is harmless.
            print(f"Port autodetect skipped: {exc}")
            port = None
    if port:
        # We have a port specified, wait for it to activate
        wait_for_port(port)
    else:
        # No port specified, try a generic delay
        print("Delay while uploading...")
        import time
        time.sleep(3)

    print("...done!")

env.AddPostAction("upload", wait_for_monitor_port)