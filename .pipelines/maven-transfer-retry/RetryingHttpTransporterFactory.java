// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.ci;

import java.io.EOFException;
import java.io.InterruptedIOException;
import java.net.SocketException;
import java.net.SocketTimeoutException;
import java.security.cert.CertificateException;
import javax.inject.Inject;
import javax.inject.Named;
import javax.inject.Singleton;
import javax.net.ssl.SSLHandshakeException;
import javax.net.ssl.SSLPeerUnverifiedException;
import org.apache.http.ConnectionClosedException;
import org.apache.http.TruncatedChunkException;
import org.apache.http.client.HttpResponseException;
import org.eclipse.aether.RepositorySystemSession;
import org.eclipse.aether.repository.RemoteRepository;
import org.eclipse.aether.spi.connector.transport.GetTask;
import org.eclipse.aether.spi.connector.transport.PeekTask;
import org.eclipse.aether.spi.connector.transport.PutTask;
import org.eclipse.aether.spi.connector.transport.Transporter;
import org.eclipse.aether.spi.connector.transport.TransporterFactory;
import org.eclipse.aether.transfer.NoTransporterException;
import org.eclipse.aether.transfer.TransferCancelledException;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

@Named("retrying-http")
@Singleton
public final class RetryingHttpTransporterFactory implements TransporterFactory {
    private static final Logger LOGGER = LoggerFactory.getLogger(RetryingHttpTransporterFactory.class);
    private static final int MAX_RETRIES = 2;
    private final TransporterFactory delegate;

    @Inject
    public RetryingHttpTransporterFactory(@Named("http") TransporterFactory delegate) {
        this.delegate = delegate;
    }

    @Override
    public float getPriority() {
        return delegate.getPriority() + 1;
    }

    @Override
    public Transporter newInstance(RepositorySystemSession session, RemoteRepository repository)
            throws NoTransporterException {
        final Transporter transport = delegate.newInstance(session, repository);
        return new Transporter() {
            @Override
            public int classify(Throwable error) {
                return transport.classify(error);
            }

            @Override
            public void peek(PeekTask task) throws Exception {
                transport.peek(task);
            }

            @Override
            public void get(GetTask task) throws Exception {
                for (int retry = 0; ; retry++) {
                    try {
                        transport.get(task);
                        return;
                    } catch (Exception failure) {
                        if (retry == MAX_RETRIES || Thread.currentThread().isInterrupted() || !retryable(failure)) {
                            throw failure;
                        }
                        LOGGER.warn("Retrying interrupted Maven download {} (retry {}/{}) after {}",
                                task.getLocation(), retry + 1, MAX_RETRIES, failure.getClass().getSimpleName());
                        try {
                            Thread.sleep(1000L * (retry + 1));
                        } catch (InterruptedException interrupted) {
                            Thread.currentThread().interrupt();
                            throw interrupted;
                        }
                    }
                }
            }

            @Override
            public void put(PutTask task) throws Exception {
                transport.put(task);
            }

            @Override
            public void close() {
                transport.close();
            }
        };
    }

    private static boolean retryable(Throwable failure) {
        boolean connectionFailure = false;
        for (Throwable cause = failure; cause != null; cause = cause.getCause()) {
            if (cause instanceof TransferCancelledException || cause instanceof InterruptedException
                    || cause instanceof HttpResponseException || cause instanceof SSLHandshakeException
                    || cause instanceof SSLPeerUnverifiedException || cause instanceof CertificateException
                    || (cause instanceof InterruptedIOException && !(cause instanceof SocketTimeoutException))) {
                return false;
            }
            connectionFailure |= cause instanceof SocketException || cause instanceof SocketTimeoutException
                    || cause instanceof EOFException || cause instanceof ConnectionClosedException
                    || cause instanceof TruncatedChunkException;
        }
        return connectionFailure;
    }
}
