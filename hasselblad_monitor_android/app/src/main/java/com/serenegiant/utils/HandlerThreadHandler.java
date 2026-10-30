package com.serenegiant.utils;

import android.os.Handler;
import android.os.HandlerThread;
import android.os.Process;

/**
 * Compatibility class: AndroidUSBCamera's USBMonitor depends on this class from the common library at runtime.
 * That library is not published in any repository (the transitive dependency is excluded); this preserves its semantics and package name.
 */
public class HandlerThreadHandler extends Handler {

    private HandlerThreadHandler(final HandlerThread thread) {
        super(thread.getLooper());
    }

    public static final HandlerThreadHandler createHandler(final String name) {
        return createHandler(name, Process.THREAD_PRIORITY_DEFAULT);
    }

    public static final HandlerThreadHandler createHandler(final String name, final int priority) {
        final HandlerThread thread = new HandlerThread(name, priority);
        thread.start();
        return new HandlerThreadHandler(thread);
    }
}
