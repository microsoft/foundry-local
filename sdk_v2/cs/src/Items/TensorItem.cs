// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

using System.Runtime.InteropServices;

using Microsoft.AI.Foundry.Local.Detail.Interop;
using Microsoft.AI.Foundry.Local.Detail.Native;

namespace Microsoft.AI.Foundry.Local;

public sealed class TensorItem : Item
{
    public FlTensorDataType DataType { get; }

    /// <summary>
    /// Pointer to the tensor's native data buffer. The buffer is owned by this
    /// <see cref="TensorItem"/> and remains valid only until the item is disposed
    /// (or ownership is transferred via <see cref="Item.ReleaseOwnership"/>).
    /// Prefer the typed span accessors (<see cref="AsSpan{T}"/>) or
    /// <see cref="ToArray{T}"/> for managed access.
    /// </summary>
    public IntPtr Data { get; }

    public long[] Shape { get; }

    /// <summary>Total number of elements in the tensor (product of <see cref="Shape"/>).</summary>
    public int ElementCount { get; }

    /// <summary>
    /// Zero-copy read-only view over the tensor's data, typed as <typeparamref name="T"/>. The
    /// returned span is valid only while this item is alive — do not retain it past disposal.
    /// </summary>
    /// <typeparam name="T">
    /// The element type. Must be an unmanaged blittable type matching <see cref="DataType"/>.
    /// Supported: <see cref="float"/>, <see cref="double"/>, <see cref="byte"/>, <see cref="sbyte"/>,
    /// <see cref="short"/>, <see cref="ushort"/>, <see cref="int"/>, <see cref="uint"/>,
    /// <see cref="long"/>, <see cref="ulong"/>, <see cref="bool"/>, and (on net8.0+) <c>Half</c>
    /// for Float16. String, complex, sub-byte, and FP8 tensors are not supported through this
    /// API; use <see cref="Data"/> directly.
    /// </typeparam>
    /// <exception cref="InvalidOperationException">If <typeparamref name="T"/> does not match <see cref="DataType"/>.</exception>
    /// <exception cref="NotSupportedException">If <typeparamref name="T"/> has no <see cref="FlTensorDataType"/> mapping.</exception>
    public ReadOnlySpan<T> AsSpan<T>() where T : unmanaged
    {
        var expected = ExpectedDataTypeFor<T>();
        if (DataType != expected)
        {
            throw new InvalidOperationException(
                $"Tensor has DataType {DataType}; AsSpan<{typeof(T).Name}>() requires {expected}.");
        }

        unsafe { return new ReadOnlySpan<T>((void*)Data, ElementCount); }
    }

    /// <summary>
    /// Copy the tensor's data into a new managed array of <typeparamref name="T"/>. Use this when
    /// you need the values to outlive the item.
    /// </summary>
    public T[] ToArray<T>() where T : unmanaged => AsSpan<T>().ToArray();

    private static FlTensorDataType ExpectedDataTypeFor<T>() where T : unmanaged
    {
        var t = typeof(T);

        return t switch
        {
            _ when t == typeof(float) => FlTensorDataType.Float,
            _ when t == typeof(double) => FlTensorDataType.Double,
            _ when t == typeof(byte) => FlTensorDataType.UInt8,
            _ when t == typeof(sbyte) => FlTensorDataType.Int8,
            _ when t == typeof(short) => FlTensorDataType.Int16,
            _ when t == typeof(ushort) => FlTensorDataType.UInt16,
            _ when t == typeof(int) => FlTensorDataType.Int32,
            _ when t == typeof(uint) => FlTensorDataType.UInt32,
            _ when t == typeof(long) => FlTensorDataType.Int64,
            _ when t == typeof(ulong) => FlTensorDataType.UInt64,
            _ when t == typeof(bool) => FlTensorDataType.Bool,
#if !NETSTANDARD2_0
            _ when t == typeof(Half) => FlTensorDataType.Float16,
#endif
            _ => throw new NotSupportedException(
                $"Element type {t.Name} has no FlTensorDataType mapping. Use the raw Data pointer for unsupported tensor types."),
        };
    }

    internal TensorItem(IntPtr ptr, bool ownsHandle) : base(ptr, ownsHandle)
    {
        var status = Api.Item.GetTensor(Ptr, out var tensor);
        Api.CheckStatus(status);

        DataType = tensor.DataType;
        Data = tensor.Data;

        var rankInt = (int)(ulong)tensor.Rank;
        Shape = new long[rankInt];
        Marshal.Copy(tensor.Shape, Shape, 0, rankInt);

        long total = 1;
        for (int i = 0; i < rankInt; i++)
        {
            total *= Shape[i];
        }

        ElementCount = checked((int)total);
    }
}
