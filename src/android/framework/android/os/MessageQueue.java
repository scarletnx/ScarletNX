// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

import java.util.ArrayList;

/**
 * A looper's messages, by the time they are due. Synchronization barriers hold back the synchronous messages behind
 * them, as Choreographer uses them. The wait for the next message is a Waiter: a monitor wait, or for the main
 * thread the window's event queue (snx.android.ui), so that input and frames wake it as Android's native Looper's
 * epoll does.
 */
public final class MessageQueue {
    /** How a queue's thread waits, and how another thread wakes it. */
    public interface Waiter {
        /** Waits up to timeout milliseconds (forever when negative), until woken. Called holding the queue. */
        void await(MessageQueue queue, long timeout) throws InterruptedException;

        /** Wakes the waiting thread. Called holding the queue. */
        void wake(MessageQueue queue);
    }

    static final Waiter MONITOR = new Waiter() {
        @Override public void await(MessageQueue queue, long timeout) throws InterruptedException {
            if (timeout < 0) queue.wait(); else if (timeout > 0) queue.wait(timeout);
        }
        @Override public void wake(MessageQueue queue) { queue.notifyAll(); }
    };

    public interface IdleHandler {
        boolean queueIdle();
    }

    public interface OnFileDescriptorEventListener {
        int EVENT_INPUT = 1;
        int EVENT_OUTPUT = 2;
        int EVENT_ERROR = 4;

        int onFileDescriptorEvents(java.io.FileDescriptor fd, int events);
    }

    private final boolean quitAllowed;
    private Message messages;
    private boolean quitting;
    private int nextBarrierToken;
    private final ArrayList<IdleHandler> idleHandlers = new ArrayList<>();
    private Waiter waiter = MONITOR;
    private boolean blocked;

    MessageQueue(boolean quitAllowed) {
        this.quitAllowed = quitAllowed;
    }

    /** The waiter of the queue's thread: the window layer sets the main thread's. */
    public synchronized void setWaiter(Waiter w) {
        waiter = w != null ? w : MONITOR;
    }

    public synchronized boolean isIdle() {
        return messages == null || SystemClock.uptimeMillis() < messages.when;
    }

    public void addIdleHandler(IdleHandler handler) {
        if (handler == null) throw new NullPointerException("Can't add a null IdleHandler");
        synchronized (this) { idleHandlers.add(handler); }
    }

    public void removeIdleHandler(IdleHandler handler) {
        synchronized (this) { idleHandlers.remove(handler); }
    }

    public void addOnFileDescriptorEventListener(java.io.FileDescriptor fd, int events,
                                                 OnFileDescriptorEventListener listener) {
        throw new UnsupportedOperationException("snx: MessageQueue file descriptor events");
    }

    public void removeOnFileDescriptorEventListener(java.io.FileDescriptor fd) {}

    /** The next message to dispatch, waiting for it; null when the looper quits. */
    Message next() {
        int pendingIdle = -1; // the idle handlers run once each time the queue goes idle
        long timeout = 0;
        for (;;) {
            ArrayList<IdleHandler> idle;
            synchronized (this) {
                if (timeout != 0 && !quitting) {
                    blocked = true;
                    try {
                        waiter.await(this, timeout);
                    } catch (InterruptedException e) {
                        // a wake: look again
                    } finally {
                        blocked = false;
                    }
                }
                if (quitting) return null;
                long now = SystemClock.uptimeMillis();
                Message prev = null, msg = messages;
                if (msg != null && msg.target == null) { // a barrier: the first asynchronous message after it
                    do {
                        prev = msg;
                        msg = msg.next;
                    } while (msg != null && !msg.asynchronous);
                }
                if (msg != null && now >= msg.when) {
                    if (prev != null) prev.next = msg.next; else messages = msg.next;
                    msg.next = null;
                    msg.inUse = true;
                    return msg;
                }
                timeout = msg != null ? msg.when - now : -1;
                if (pendingIdle < 0 && (messages == null || now < messages.when)) pendingIdle = idleHandlers.size();
                if (pendingIdle <= 0) continue;
                idle = new ArrayList<>(idleHandlers);
            }
            for (IdleHandler h : idle) {
                boolean keep;
                try {
                    keep = h.queueIdle();
                } catch (Throwable t) {
                    android.util.Log.wtf("MessageQueue", "IdleHandler threw exception", t);
                    keep = false;
                }
                if (!keep) synchronized (this) { idleHandlers.remove(h); }
            }
            pendingIdle = 0;
            timeout = 0; // an idle handler may have posted a message
        }
    }

    void quit(boolean safe) {
        if (!quitAllowed) throw new IllegalStateException("Main thread not allowed to quit.");
        synchronized (this) {
            if (quitting) return;
            quitting = true;
            if (safe) {
                long now = SystemClock.uptimeMillis();
                Message p = messages, prev = null;
                while (p != null && p.when <= now) { prev = p; p = p.next; }
                if (prev != null) prev.next = null; else messages = null;
            } else {
                messages = null;
            }
            waiter.wake(this);
        }
    }

    /** A barrier that holds back the synchronous messages after when, until removeSyncBarrier. */
    public int postSyncBarrier() {
        return postSyncBarrier(SystemClock.uptimeMillis());
    }

    private int postSyncBarrier(long when) {
        synchronized (this) {
            int token = nextBarrierToken++;
            Message msg = Message.obtain();
            msg.inUse = true;
            msg.when = when;
            msg.arg1 = token;
            Message prev = null, p = messages;
            if (when != 0) while (p != null && p.when <= when) { prev = p; p = p.next; }
            msg.next = p;
            if (prev != null) prev.next = msg; else messages = msg;
            return token;
        }
    }

    public void removeSyncBarrier(int token) {
        synchronized (this) {
            Message prev = null, p = messages;
            while (p != null && (p.target != null || p.arg1 != token)) { prev = p; p = p.next; }
            if (p == null) throw new IllegalStateException("The specified message queue synchronization barrier token has not been posted or has already been removed.");
            if (prev != null) prev.next = p.next; else messages = p.next;
            waiter.wake(this);
        }
    }

    boolean enqueueMessage(Message msg, long when) {
        if (msg.target == null) throw new IllegalArgumentException("Message must have a target.");
        synchronized (this) {
            if (quitting) {
                android.util.Log.w("MessageQueue", msg.target + " sending message to a Handler on a dead thread");
                return false;
            }
            msg.inUse = true;
            msg.when = when;
            Message p = messages, prev = null;
            if (p == null || when == 0 || when < p.when) {
                msg.next = p;
                messages = msg;
            } else {
                while (p != null && p.when <= when) { prev = p; p = p.next; }
                msg.next = p;
                prev.next = msg;
            }
            waiter.wake(this);
        }
        return true;
    }

    boolean hasMessages(Handler h, int what, Object object) {
        if (h == null) return false;
        synchronized (this) {
            for (Message p = messages; p != null; p = p.next)
                if (p.target == h && p.what == what && (object == null || p.obj == object)) return true;
            return false;
        }
    }

    boolean hasMessages(Handler h, Runnable r, Object object) {
        if (h == null) return false;
        synchronized (this) {
            for (Message p = messages; p != null; p = p.next)
                if (p.target == h && p.callback == r && (object == null || p.obj == object)) return true;
            return false;
        }
    }

    boolean hasMessages(Handler h) {
        if (h == null) return false;
        synchronized (this) {
            for (Message p = messages; p != null; p = p.next) if (p.target == h) return true;
            return false;
        }
    }

    interface Match { boolean test(Message m); }

    void removeMatching(Match match) {
        synchronized (this) {
            Message prev = null, p = messages;
            while (p != null) {
                Message n = p.next;
                if (p.target != null && match.test(p)) {
                    if (prev != null) prev.next = n; else messages = n;
                } else {
                    prev = p;
                }
                p = n;
            }
        }
    }

    void removeMessages(Handler h, int what, Object object) {
        if (h != null) removeMatching(p -> p.target == h && p.what == what && (object == null || p.obj == object));
    }

    void removeMessages(Handler h, Runnable r, Object object) {
        if (h != null && r != null)
            removeMatching(p -> p.target == h && p.callback == r && (object == null || p.obj == object));
    }

    void removeCallbacksAndMessages(Handler h, Object object) {
        if (h != null) removeMatching(p -> p.target == h && (object == null || p.obj == object));
    }
}
