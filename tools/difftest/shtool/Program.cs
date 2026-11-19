// Decompiles a Maxwell shader (SPH + code, as in a DKSH program) to GLSL with Ryujinx's translator (debug comments
// carry the disassembly). Usage: shtool <binary> [compute]
using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.IO;
using System.Runtime.InteropServices;

class Accessor : IGpuAccessor
{
    private readonly byte[] _data;
    private int _textures, _images;
    public Accessor(byte[] data) { _data = data; }
    public SetBindingPair CreateConstantBufferBinding(int index) => new SetBindingPair(0, index + 1);
    public SetBindingPair CreateImageBinding(int count, bool isBuffer) { int b = _images; _images += count; return new SetBindingPair(3, b); }
    public SetBindingPair CreateStorageBufferBinding(int index) => new SetBindingPair(1, index);
    public SetBindingPair CreateTextureBinding(int count, bool isBuffer) { int b = _textures; _textures += count; return new SetBindingPair(2, b); }
    public ReadOnlySpan<ulong> GetCode(ulong address, int minimumSize) => MemoryMarshal.Cast<byte, ulong>(new ReadOnlySpan<byte>(_data)[(int)address..]);
    public int QuerySamplerArrayLengthFromPool() => 32;
    public int QueryTextureArrayLengthFromBuffer(int slot) => 32;
    public int QueryTextureArrayLengthFromPool() => 32;
}

static class Program
{
    static void Main(string[] args)
    {
        var flags = TranslationFlags.DebugMode;
        if (args.Length > 1 && args[1] == "compute") flags |= TranslationFlags.Compute;
        byte[] data = File.ReadAllBytes(args[0]);
        var options = new TranslationOptions(TargetLanguage.Glsl, TargetApi.OpenGL, flags);
        var ctx = Translator.CreateContext(0, new Accessor(data), options);
        Console.WriteLine(ctx.Translate().Code);
    }
}
