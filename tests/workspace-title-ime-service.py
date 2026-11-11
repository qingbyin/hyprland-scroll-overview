#!/usr/bin/python3
"""Isolated Fcitx transport fixture; requires system Python dbus and gi.

The control interface is deliberately separate from the emulated Fcitx API.
Delayed replies use GLib only here, never in the C++ client under test.
"""

import os
import sys

import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

NAME = "org.fcitx.Fcitx5"
ROOT = "/org/freedesktop/portal/inputmethod"
IM = "org.fcitx.Fcitx.InputMethod1"
IC = "org.fcitx.Fcitx.InputContext1"
CONTROL = "org.example.WorkspaceTitleIMETest"
CONTROL_PATH = "/org/example/WorkspaceTitleIMETest"


def later(delay, function, *args):
    if delay <= 0:
        function(*args)
    else:
        def invoke():
            function(*args)
            return GLib.SOURCE_REMOVE
        GLib.timeout_add(delay, invoke)


class Signals(dbus.service.Object):
    def __init__(self, bus, path, destination):
        super().__init__(bus, path)
        self.bus = bus
        self.path = path
        self.destination = destination

    def send_signal(self, member, signature, *args):
        # Real Fcitx signals are unicast, not broadcasts requiring AddMatch.
        message = dbus.lowlevel.SignalMessage(self.path, IC, member)
        message.set_destination(self.destination)
        message.append(*args, signature=signature)
        self.bus.send_message(message)

    def CommitString(self, text):
        self.send_signal("CommitString", "s", text)

    def ForwardKey(self, sym, modifiers, release):
        self.send_signal("ForwardKey", "uub", sym, modifiers, release)

    def UpdateClientSideUI(self, *args):
        self.send_signal("UpdateClientSideUI", "a(si)ia(si)a(si)a(ss)iibb", *args)

    def panel(self):
        self.UpdateClientSideUI(
            [("你", 0), ("ab", 0)], 5, [("up", 0)], [("down", 0)],
            [("1", "候選"), ("2", "次")], 1, 0, True, False)


class Context(Signals):
    def __init__(self, service, path, destination):
        super().__init__(service.bus, path, destination)
        self.service = service
        self.path = path
        self.last_key = (0, 0, False)
        self.last_request = None

    def startup(self, event, reply, error):
        self.service.events.append(event)
        if self.service.mode == "startup-error":
            later(self.service.startup_delay, error,
                  dbus.exceptions.DBusException(
                      "startup rejected", name=CONTROL + ".Rejected"))
        else:
            later(self.service.startup_delay, reply)

    @dbus.service.method(IC, in_signature="t", out_signature="",
                         async_callbacks=("reply", "error"))
    def SetCapability(self, capability, reply, error):
        self.startup("cap:" + str(int(capability)), reply, error)

    @dbus.service.method(IC, in_signature="", out_signature="",
                         async_callbacks=("reply", "error"))
    def FocusIn(self, reply, error):
        self.startup("focus-in", reply, error)

    @dbus.service.method(IC, in_signature="", out_signature="")
    def Reset(self):
        self.service.events.append("reset")

    @dbus.service.method(IC, in_signature="", out_signature="")
    def FocusOut(self):
        self.service.events.append("focus-out")

    @dbus.service.method(IC, in_signature="", out_signature="")
    def DestroyIC(self):
        self.service.events.append("destroy")
        # Keep the object alive so tests can send stale signals deliberately.

    @dbus.service.method(IC, in_signature="uuubu", out_signature="b",
                         async_callbacks=("reply", "error"), message_keyword="message")
    def ProcessKeyEvent(self, sym, code, modifiers, release, time, reply, error,
                        message=None):
        service = self.service
        mode = service.mode
        self.last_key = (sym, modifiers, release)
        service.events.append("key:%d:%d:%d:%d:%d" %
                              (sym, code, modifiers, release, time))
        if mode == "commit-before-timeout":
            self.CommitString("確定")
        if mode == "spoof-reply":
            if self.last_request is not None:
                # Reuse an old call to obtain a stale serial without mutating
                # message headers through dbus-python's low-level setters.
                wrong_serial = dbus.lowlevel.MethodReturnMessage(self.last_request)
                wrong_serial.append(False, signature="b")
                service.bus.send_message(wrong_serial)
            self.last_request = message
            wrong_sender = dbus.lowlevel.MethodReturnMessage(message)
            wrong_sender.append(False, signature="b")
            service.attacker.send_message(wrong_sender)
            service.attacker.flush()

        def finish():
            if mode == "key-error":
                error(dbus.exceptions.DBusException(
                    "key rejected", name=CONTROL + ".Rejected"))
                return
            if mode in ("commit", "commit-forward"):
                self.CommitString("確定")
            if mode in ("forward", "commit-forward", "forward-false"):
                self.ForwardKey(sym, modifiers, release)
            if mode == "panel":
                self.panel()
            reply(mode not in ("unhandled", "forward-false"))
            if mode == "forward-after-reply":
                # Exercise reply-before-signal order and snapshot deduplication.
                self.ForwardKey(sym, modifiers, release)
                self.ForwardKey(sym, modifiers, release)

        later(service.key_delay, finish)

    @dbus.service.method(IC, in_signature="i", out_signature="",
                         async_callbacks=("reply", "error"))
    def SelectCandidate(self, index, reply, error):
        self.service.events.append("select:" + str(int(index)))
        later(self.service.select_delay, reply)


class Service(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, ROOT)
        self.bus = bus
        self.events = []
        self.mode = "handled"
        self.create_delay = self.key_delay = 0
        self.startup_delay = self.select_delay = 0
        self.contexts = []
        self.current = None
        self.attacker = dbus.bus.BusConnection(os.environ["DBUS_SESSION_BUS_ADDRESS"])
        self.signal_objects = []

    @dbus.service.method(IM, in_signature="a(ss)", out_signature="oay",
                         async_callbacks=("reply", "error"), sender_keyword="sender")
    def CreateInputContext(self, options, reply, error, sender=None):
        self.events.append("create")
        for name, value in options:
            self.events.append("option:%s=%s" % (name, value))
        if self.mode == "create-error":
            later(self.create_delay, error, dbus.exceptions.DBusException(
                "create rejected", name=CONTROL + ".Rejected"))
            return
        context = Context(self, ROOT + "/test_" + str(len(self.contexts) + 1), sender)
        self.contexts.append(context)
        self.current = context
        later(self.create_delay, reply, dbus.ObjectPath(context.path),
              dbus.ByteArray(bytes(range(16))))


class Control(dbus.service.Object):
    def __init__(self, service, loop):
        super().__init__(service.bus, CONTROL_PATH)
        self.service = service
        self.loop = loop

    @dbus.service.method(CONTROL, in_signature="siiii", out_signature="")
    def Configure(self, mode, create_delay, key_delay, startup_delay, select_delay):
        service = self.service
        service.events.clear()
        service.mode = str(mode)
        service.create_delay = int(create_delay)
        service.key_delay = int(key_delay)
        service.startup_delay = int(startup_delay)
        service.select_delay = int(select_delay)

    @dbus.service.method(CONTROL, in_signature="", out_signature="as")
    def Events(self):
        return dbus.Array(self.service.events, signature="s")

    @dbus.service.method(CONTROL, in_signature="s", out_signature="")
    def Emit(self, kind):
        service = self.service
        context = service.current
        if context is None:
            raise dbus.exceptions.DBusException("no context")
        if kind == "panel":
            context.panel()
        elif kind == "commit":
            context.CommitString("確定")
        elif kind == "malformed":
            # Correct owner, interface and path; only the signatures are wrong.
            for member, signature, args in [
                ("CommitString", "", ()),
                ("CommitString", "u", (42,)),
                ("CommitString", "ss", ("bad", "extra")),
                ("ForwardKey", "", ()),
                ("ForwardKey", "uuu", (97, 5, 0)),
                ("ForwardKey", "uu", (97, 5)),
                ("UpdateClientSideUI", "", ()),
                ("UpdateClientSideUI", "s", ("bad",)),
                ("UpdateClientSideUI", "a(si)", ([("truncated", 0)],)),
                ("UpdateClientSideUI", "a(si)ia(si)a(si)a(si)iibb",
                 ([("bad", 0)], 0, [], [], [("wrong candidate type", 1)],
                  0, 0, False, False)),
            ]:
                context.send_signal(member, signature, *args)
        elif kind == "foreign":
            fake = Signals(service.attacker, context.path, context.destination)
            fake.CommitString("FORGED")
            fake.panel()
            fake.ForwardKey(*context.last_key)
            service.attacker.flush()
            fake.remove_from_connection()
        elif kind == "wrong-path":
            fake = Signals(service.bus, context.path + "/wrong", context.destination)
            fake.CommitString("FORGED")
            fake.panel()
            fake.ForwardKey(*context.last_key)
            service.bus.flush()
            fake.remove_from_connection()
        elif kind == "stale":
            old = service.contexts[-2]
            destination = old.destination
            old.destination = context.destination
            old.CommitString("STALE")
            old.panel()
            old.ForwardKey(*old.last_key)
            old.destination = destination
        else:
            raise dbus.exceptions.DBusException("unknown signal kind")
        service.bus.flush()

    @dbus.service.method(CONTROL, in_signature="", out_signature="")
    def ReleaseName(self):
        self.service.bus.release_name(NAME)

    @dbus.service.method(CONTROL, in_signature="", out_signature="")
    def AcquireName(self):
        self.service.bus.request_name(NAME, dbus.bus.NAME_FLAG_DO_NOT_QUEUE)

    @dbus.service.method(CONTROL, in_signature="", out_signature="")
    def Quit(self):
        GLib.idle_add(self.loop.quit)


if __name__ == "__main__":
    DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus(private=True)
    bus.request_name(NAME, dbus.bus.NAME_FLAG_DO_NOT_QUEUE)
    control_name = dbus.service.BusName(CONTROL, bus=bus, do_not_queue=True)
    loop = GLib.MainLoop()
    service = Service(bus)
    control = Control(service, loop)
    if len(sys.argv) > 1:
        with open(sys.argv[1], "w", encoding="utf-8") as ready:
            ready.write("ready\n")
    loop.run()
