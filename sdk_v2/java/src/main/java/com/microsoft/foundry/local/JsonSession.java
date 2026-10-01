// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.jna.Memory;
import com.sun.jna.Pointer;
import com.sun.jna.ptr.PointerByReference;

/** Shared native OpenAI-JSON request bridge for non-generative sessions. */
abstract class JsonSession extends OwnedSession {
    private static final ObjectMapper JSON = new ObjectMapper();

    final Model model;
    final NativeApi api;
    private Pointer handle;

    JsonSession(Model model, String expectedTask, String sessionName) {
        NativeApi.outsideCallback();
        synchronized (model.owner) {
            model.owner.checkOpen();
            String actualTask = model.info().task();
            if (!expectedTask.equals(actualTask)) {
                throw new IllegalStateException(
                        sessionName + " requires task '" + expectedTask + "', but got '" + actualTask + "'");
            }
            this.model = model;
            api = model.owner.api;
            handle = api.create(api.inference, NativeApi.InferenceApi.SESSION_CREATE, model.handle);
            model.owner.sessions.add(this);
        }
    }

    @Override Model model() {
        return model;
    }

    final <T> T process(Object input, Class<T> outputType) {
        NativeApi.outsideCallback();
        final String requestJson;
        try {
            requestJson = JSON.writeValueAsString(input);
        } catch (JsonProcessingException e) {
            throw new IllegalArgumentException("Cannot serialize non-generative request", e);
        }

        synchronized (model.owner) {
            model.owner.checkOpen();
            if (handle == null) throw new IllegalStateException("Session is closed");

            Pointer request = api.create(api.inference, NativeApi.InferenceApi.REQUEST_CREATE);
            Pointer item = null;
            Pointer response = null;
            boolean requestOwnsItem = false;
            try {
                item = api.create(
                        api.item, NativeApi.ItemApi.CREATE, NativeApi.ItemApi.TYPE_TEXT);
                Memory text = NativeApi.utf8(requestJson);
                NativeApi.TextData data = new NativeApi.TextData();
                data.text = text;
                data.type = NativeApi.ItemApi.TEXT_OPENAI_JSON;
                data.write();
                api.check(api.item.pointer(NativeApi.ItemApi.SET_TEXT, item, data));

                requestOwnsItem = true;
                api.check(api.inference.pointer(
                        NativeApi.InferenceApi.REQUEST_ADD_ITEM, request, item, (byte) 1));

                PointerByReference output = new PointerByReference();
                Pointer status = api.inference.pointer(
                        NativeApi.InferenceApi.SESSION_PROCESS_REQUEST, handle, request, output);
                response = output.getValue();
                api.check(status);
                if (response == null) {
                    throw new IllegalStateException("Native session returned no response");
                }
                if (api.inference.size(NativeApi.InferenceApi.RESPONSE_GET_ITEM_COUNT, response) != 1) {
                    throw new IllegalStateException(
                            "Non-generative session returned an invalid response item count");
                }
                Pointer responseItem = api.output(
                        api.inference, NativeApi.InferenceApi.RESPONSE_GET_ITEM, response, 0L);
                if (api.item.integer(NativeApi.ItemApi.GET_TYPE, responseItem)
                        != NativeApi.ItemApi.TYPE_TEXT) {
                    throw new IllegalStateException(
                            "Non-generative session returned a non-text response");
                }
                NativeApi.TextData responseText = new NativeApi.TextData();
                responseText.write();
                api.check(api.item.pointer(
                        NativeApi.ItemApi.GET_TEXT, responseItem, responseText));
                responseText.read();
                if (responseText.type != NativeApi.ItemApi.TEXT_OPENAI_JSON) {
                    throw new IllegalStateException(
                            "Non-generative session returned a non-JSON response");
                }
                try {
                    return JSON.readValue(NativeApi.text(responseText.text), outputType);
                } catch (JsonProcessingException e) {
                    throw new IllegalStateException(
                            "Cannot deserialize non-generative response", e);
                }
            } finally {
                if (response != null) {
                    api.inference.call(NativeApi.InferenceApi.RESPONSE_RELEASE, response);
                }
                api.inference.call(NativeApi.InferenceApi.REQUEST_RELEASE, request);
                if (item != null && !requestOwnsItem) {
                    api.item.call(NativeApi.ItemApi.RELEASE, item);
                }
            }
        }
    }

    static String writeJson(Object value) {
        try {
            return JSON.writeValueAsString(value);
        } catch (JsonProcessingException e) {
            throw new IllegalArgumentException(e);
        }
    }

    static <T> T readJson(String value, Class<T> type) {
        try {
            return JSON.readValue(value, type);
        } catch (JsonProcessingException e) {
            throw new IllegalArgumentException(e);
        }
    }

    @Override public final void close() {
        NativeApi.outsideCallback();
        synchronized (model.owner) {
            if (handle == null) return;
            try {
                api.inference.call(NativeApi.InferenceApi.SESSION_RELEASE, handle);
            } finally {
                handle = null;
                model.owner.sessions.remove(this);
            }
        }
    }
}
