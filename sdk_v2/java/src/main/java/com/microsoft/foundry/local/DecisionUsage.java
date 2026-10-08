// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import com.fasterxml.jackson.annotation.JsonProperty;

/** Usage returned by a typed-decision request. */
public record DecisionUsage(@JsonProperty("billing_units") long billingUnits) {}
