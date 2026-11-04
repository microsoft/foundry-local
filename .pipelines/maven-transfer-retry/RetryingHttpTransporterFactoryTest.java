// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.ci;

import java.io.EOFException;
import java.io.IOException;
import java.io.InterruptedIOException;
import java.net.SocketException;
import java.net.SocketTimeoutException;
import java.net.URI;
import java.nio.file.AccessDeniedException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import javax.net.ssl.SSLHandshakeException;
import javax.net.ssl.SSLPeerUnverifiedException;
import org.apache.http.ConnectionClosedException;
import org.apache.http.TruncatedChunkException;
import org.apache.http.client.HttpResponseException;
import org.eclipse.aether.DefaultRepositorySystemSession;
import org.eclipse.aether.RepositorySystemSession;
import org.eclipse.aether.repository.RemoteRepository;
import org.eclipse.aether.spi.connector.transport.GetTask;
import org.eclipse.aether.spi.connector.transport.PeekTask;
import org.eclipse.aether.spi.connector.transport.PutTask;
import org.eclipse.aether.spi.connector.transport.Transporter;
import org.eclipse.aether.spi.connector.transport.TransporterFactory;
import org.eclipse.aether.transfer.TransferCancelledException;
import org.eclipse.aether.transport.http.HttpTransporterFactory;

public final class RetryingHttpTransporterFactoryTest {
    private static final GetTask TASK = new GetTask(URI.create("artifact.jar"));

    private static final class Stub implements Transporter {
        private final Exception failure;
        private final int failures;
        private int calls;

        Stub(Exception failure, int failures) {
            this.failure = failure;
            this.failures = failures;
        }

        public int classify(Throwable error) { return ERROR_OTHER; }
        public void peek(PeekTask task) {}
        public void put(PutTask task) {}
        public void close() {}

        public void get(GetTask task) throws Exception {
            if (++calls <= failures) {
                throw failure;
            }
        }
    }

    private static Transporter wrap(final Stub stub) throws Exception {
        return new RetryingHttpTransporterFactory(new TransporterFactory() {
            public float getPriority() { return 5; }
            public Transporter newInstance(RepositorySystemSession session, RemoteRepository repository) {
                return stub;
            }
        }).newInstance(null, null);
    }

    private static void check(boolean condition, String message) {
        if (!condition) {
            throw new AssertionError(message);
        }
    }

    private static void noRetry(Exception failure) throws Exception {
        Stub stub = new Stub(failure, Integer.MAX_VALUE);
        try {
            wrap(stub).get(TASK);
            throw new AssertionError("Expected failure: " + failure);
        } catch (Exception actual) {
            check(actual == failure, "Original exception was not preserved");
            check(stub.calls == 1, "Retried permanent failure: " + failure);
        }
    }

    public static void main(String[] args) throws Exception {
        RetryingHttpTransporterFactory nativeFactory = new RetryingHttpTransporterFactory(new HttpTransporterFactory());
        for (String scheme : new String[] {"http", "https"}) {
            RemoteRepository repository = new RemoteRepository.Builder("fixture", "default", scheme + "://localhost")
                    .build();
            try (Transporter transport = nativeFactory.newInstance(new DefaultRepositorySystemSession(), repository)) {
                check(transport.getClass().getEnclosingClass() == RetryingHttpTransporterFactory.class,
                        "Native transport was not decorated for " + scheme);
            }
        }
        for (Exception failure : new Exception[] {
                new EOFException(), new SocketException("Connection reset"), new SocketTimeoutException(),
                new ConnectionClosedException("Truncated body"), new TruncatedChunkException("Truncated chunk")
        }) {
            Stub stub = new Stub(failure, 1);
            wrap(stub).get(TASK);
            check(stub.calls == 2, "Did not recover from " + failure);
        }
        Stub exhausted = new Stub(new ConnectionClosedException("Always truncated"), Integer.MAX_VALUE);
        try {
            wrap(exhausted).get(TASK);
            throw new AssertionError("Persistent failure passed");
        } catch (ConnectionClosedException expected) {
            check(exhausted.calls == 3, "Retry budget was not bounded");
        }
        SSLHandshakeException certificate = new SSLHandshakeException("Untrusted certificate");
        certificate.initCause(new SocketException("Must not override TLS failure"));
        HttpResponseException authentication = new HttpResponseException(401, "Unauthorized");
        authentication.initCause(new SocketException("Must not override HTTP status"));
        for (Exception failure : new Exception[] {
                authentication, new HttpResponseException(403, "Forbidden"), new HttpResponseException(404, "Not found"),
                certificate, new SSLPeerUnverifiedException("Wrong hostname"), new AccessDeniedException("artifact"),
                new IOException("Disk failure"), new InterruptedIOException("Interrupted"),
                new TransferCancelledException("Cancelled"), new InterruptedException("Interrupted")
        }) {
            noRetry(failure);
        }
        Thread.currentThread().interrupt();
        try {
            noRetry(new ConnectionClosedException("Interrupted transfer"));
            check(Thread.currentThread().isInterrupted(), "Lost interrupt status");
        } finally {
            Thread.interrupted();
        }

        Stub interrupted = new Stub(new ConnectionClosedException("Backoff"), Integer.MAX_VALUE);
        AtomicBoolean preservedInterrupt = new AtomicBoolean();
        AtomicReference<Throwable> unexpected = new AtomicReference<>();
        Thread worker = new Thread(() -> {
            try {
                wrap(interrupted).get(TASK);
                unexpected.set(new AssertionError("Interrupted transfer passed"));
            } catch (InterruptedException expected) {
                preservedInterrupt.set(Thread.currentThread().isInterrupted());
            } catch (Throwable failure) {
                unexpected.set(failure);
            }
        });
        worker.setDaemon(true);
        worker.start();
        try {
            long deadline = System.nanoTime() + 2_000_000_000L;
            while (worker.getState() != Thread.State.TIMED_WAITING && System.nanoTime() < deadline) {
                Thread.sleep(10);
            }
            check(worker.getState() == Thread.State.TIMED_WAITING, "Did not enter retry backoff");
        } finally {
            worker.interrupt();
            worker.join(2000);
        }
        check(!worker.isAlive(), "Cancellation did not terminate promptly");
        check(unexpected.get() == null, "Unexpected cancellation result: " + unexpected.get());
        check(preservedInterrupt.get() && interrupted.calls == 1, "Retried cancellation or lost interrupt status");
        System.out.println("Transport retry policy tests passed");
    }
}
