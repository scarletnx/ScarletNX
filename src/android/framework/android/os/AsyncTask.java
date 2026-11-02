// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

import java.util.concurrent.Executor;
import java.util.concurrent.Executors;
import java.util.concurrent.FutureTask;
import java.util.concurrent.atomic.AtomicBoolean;

/** A background task with its results on the main thread, in Android's order of callbacks. Deprecated, still used. */
@Deprecated
public abstract class AsyncTask<Params, Progress, Result> {
    public enum Status { PENDING, RUNNING, FINISHED }

    public static final Executor THREAD_POOL_EXECUTOR = Executors.newCachedThreadPool(r -> {
        Thread t = new Thread(r, "AsyncTask");
        t.setDaemon(true);
        return t;
    });
    /** One task at a time, in order, as Android's default executor runs them */
    public static final Executor SERIAL_EXECUTOR = Executors.newSingleThreadExecutor(r -> {
        Thread t = new Thread(r, "AsyncTask #1");
        t.setDaemon(true);
        return t;
    });

    private static final Handler main = new Handler(Looper.getMainLooper());
    private volatile Status status = Status.PENDING;
    private final AtomicBoolean cancelled = new AtomicBoolean();
    private FutureTask<Result> future;

    public AsyncTask() {}

    protected abstract Result doInBackground(Params... params);
    protected void onPreExecute() {}
    protected void onPostExecute(Result result) {}
    protected void onProgressUpdate(Progress... values) {}
    protected void onCancelled(Result result) { onCancelled(); }
    protected void onCancelled() {}

    public final Status getStatus() { return status; }
    public final boolean isCancelled() { return cancelled.get(); }
    public final boolean cancel(boolean mayInterruptIfRunning) {
        cancelled.set(true);
        return future != null && future.cancel(mayInterruptIfRunning);
    }
    public final Result get() throws InterruptedException, java.util.concurrent.ExecutionException { return future.get(); }
    public final Result get(long timeout, java.util.concurrent.TimeUnit unit)
        throws InterruptedException, java.util.concurrent.ExecutionException, java.util.concurrent.TimeoutException {
        return future.get(timeout, unit);
    }

    @SafeVarargs
    public final AsyncTask<Params, Progress, Result> execute(Params... params) { return executeOnExecutor(SERIAL_EXECUTOR, params); }
    @SafeVarargs
    public final AsyncTask<Params, Progress, Result> executeOnExecutor(Executor exec, Params... params) {
        if (status != Status.PENDING)
            throw new IllegalStateException(status == Status.RUNNING ? "Cannot execute task: the task is already running."
                : "Cannot execute task: the task has already been executed (a task can be executed only once)");
        status = Status.RUNNING;
        onPreExecute();
        future = new FutureTask<>(() -> doInBackground(params)) {
            @Override protected void done() {
                Result result = null;
                try { result = get(); } catch (Exception ignored) {}
                Result r = result;
                main.post(() -> finish(r));
            }
        };
        exec.execute(future);
        return this;
    }
    public static void execute(Runnable runnable) { SERIAL_EXECUTOR.execute(runnable); }

    @SafeVarargs
    protected final void publishProgress(Progress... values) {
        if (!isCancelled()) main.post(() -> onProgressUpdate(values));
    }
    private void finish(Result result) {
        if (isCancelled()) onCancelled(result);
        else onPostExecute(result);
        status = Status.FINISHED;
    }
}
