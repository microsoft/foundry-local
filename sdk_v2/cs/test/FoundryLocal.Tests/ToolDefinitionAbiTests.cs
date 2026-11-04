// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;

using System.Runtime.InteropServices;
using System.Threading.Tasks;

using Microsoft.AI.Foundry.Local.Detail.Interop;

// These constants intentionally pin the managed ABI to the C header values.
#pragma warning disable TUnitAssertions0005

/// <summary>
/// The C# half of the flToolDefinition ABI contract. The native library reads the struct this
/// binding fills in at fixed offsets and only reads <c>kind</c> when the stamped version says the
/// field is there, so the layout mirrored here and the version stamped on it are both load-bearing:
/// a mismatch would be read as garbage rather than rejected. These assertions mirror the C++
/// <c>tool_definition_abi_test.cc</c> static_asserts and the Python
/// <c>test_tool_definition_abi.py</c> offsets, and require neither the native library nor a model.
/// </summary>
internal sealed class ToolDefinitionAbiTests
{
    // The struct is `uint32_t version` followed by three pointers and then `kind`. Every offset is
    // therefore a multiple of the pointer size on both 32- and 64-bit: the 4-byte version is
    // followed by 4 bytes of padding only where pointers are 8-byte aligned. On x64 that is
    // 0/8/16/24/32 with a total size of 40 — the numbers the C header static_asserts — and on a
    // 32-bit target it is 0/4/8/12/16 with a total size of 20.
    private static int Slot => IntPtr.Size;

    [Test]
    public async Task Layout_KindIsAppendedAfterTheUnchangedLegacyPrefix()
    {
        await Assert.That(Marshal.OffsetOf<FlToolDefinition>(nameof(FlToolDefinition.Version)).ToInt64())
            .IsEqualTo(0L);
        await Assert.That(Marshal.OffsetOf<FlToolDefinition>(nameof(FlToolDefinition.Name)).ToInt64())
            .IsEqualTo((long)Slot);
        await Assert.That(Marshal.OffsetOf<FlToolDefinition>(nameof(FlToolDefinition.Description)).ToInt64())
            .IsEqualTo((long)(2 * Slot));
        await Assert.That(Marshal.OffsetOf<FlToolDefinition>(nameof(FlToolDefinition.JsonSchema)).ToInt64())
            .IsEqualTo((long)(3 * Slot));

        // `kind` sits immediately after the version 1 prefix, which is what lets the native side
        // read an older, smaller definition without walking off its end.
        await Assert.That(Marshal.OffsetOf<FlToolDefinition>(nameof(FlToolDefinition.Kind)).ToInt64())
            .IsEqualTo((long)(4 * Slot));
        await Assert.That(Marshal.SizeOf<FlToolDefinition>()).IsEqualTo(5 * Slot);

        // flToolKind is a uint32_t typedef rather than an enum, whose underlying type C leaves to
        // the implementation. The managed mirror has to pin the same width explicitly.
        await Assert.That(Enum.GetUnderlyingType(typeof(FlToolKind))).IsEqualTo(typeof(uint));
        await Assert.That((uint)FlToolKind.Function).IsEqualTo(0u);
        await Assert.That((uint)FlToolKind.Custom).IsEqualTo(1u);

        // A definition built without naming a kind is a function tool, which is what keeps the
        // function-tool entry points on every wrapper unchanged by the version 2 field.
        await Assert.That(default(FlToolDefinition).Kind).IsEqualTo(FlToolKind.Function);

        // `kind` is only read from a version 2 or later definition, so this binding must request —
        // and stamp — at least that version.
        await Assert.That(NativeMethods.ApiVersion).IsGreaterThanOrEqualTo(2u);
    }
    #pragma warning restore TUnitAssertions0005
}
