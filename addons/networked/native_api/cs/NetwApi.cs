using System;
using System.Collections.Concurrent;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using Godot;
using Godot.NativeInterop;

namespace Networked;

/// <summary>
/// The dispatch table the addon exports, loaded once on first use.
/// </summary>
/// <remarks>
/// Every generated binding calls through here. The table is fetched with a
/// single <c>ClassDB.ClassCallStatic</c> and then never again, so a call
/// costs a delegate invocation rather than a name lookup.
/// </remarks>
public static class NetwApi
{
    public const long SupportedVersion = 3;

    private static readonly string[] Exported =
    {
        "method_bind",
        "ptrcall0", "ptrcall1", "ptrcall2", "ptrcall3",
        "call0", "call1", "call2", "call3", "call4",
        "args_new", "args_set", "call_pack", "args_free",
        "retain", "release",
    };

    private delegate IntPtr MethodBindDelegate(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string className,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string methodName,
        ulong hash);

    private delegate void ReferenceDelegate(IntPtr instance);

    private static MethodBindDelegate _methodBind;
    private static Godot.Collections.Dictionary _functions;
    private static ReferenceDelegate _retain;
    private static ReferenceDelegate _release;
    private static bool _loaded;
    private static string _failure;

    /// <summary>Whether the addon answered and its table matched.</summary>
    public static bool IsAvailable
    {
        get
        {
            Load();
            return _failure == null;
        }
    }

    /// <summary>Why the table could not be used, or null.</summary>
    public static string Failure
    {
        get
        {
            Load();
            return _failure;
        }
    }

    private static void Load()
    {
        if (_loaded)
        {
            return;
        }
        _loaded = true;

        if (!ClassDB.ClassExists("Netw"))
        {
            _failure = "the Networked addon is not installed in this project";
            return;
        }

        Variant answered = ClassDB.ClassCallStatic("Netw", "_native_api");
        if (answered.VariantType != Variant.Type.Dictionary)
        {
            _failure = "Netw._native_api answered no table";
            return;
        }

        var table = answered.AsGodotDictionary();
        long version = table["version"].AsInt64();
        if (version != SupportedVersion)
        {
            _failure =
                $"the addon exports native API version {version}, and " +
                $"these bindings were generated for {SupportedVersion}";
            return;
        }

        long hash = table["hash"].AsInt64();
        if (hash != ExpectedHash())
        {
            _failure = "the addon exports a different set of native API " +
                       "functions than these bindings were generated for";
            return;
        }

        bool isDouble = table["is_double"].AsBool();
        if (isDouble != NetwBuild.IsDoublePrecision)
        {
            _failure =
                "the addon and these bindings disagree about float width";
            return;
        }

        _functions = table["functions"].AsGodotDictionary();
        _methodBind = Bind<MethodBindDelegate>(_functions, "method_bind");
        _retain = Bind<ReferenceDelegate>(_functions, "retain");
        _release = Bind<ReferenceDelegate>(_functions, "release");
    }

    private static long ExpectedHash()
    {
        ulong hash = 14695981039346656037UL;
        foreach (string name in Exported)
        {
            foreach (char at in name)
            {
                hash ^= (byte)at;
                hash *= 1099511628211UL;
            }
            hash ^= (ulong)SupportedVersion;
            hash *= 1099511628211UL;
        }
        return (long)(hash & 0x7fffffffffffffffUL);
    }

    private static T Bind<T>(
        Godot.Collections.Dictionary functions, string name)
        where T : Delegate
    {
        long address = functions[name].AsInt64();
        if (address == 0)
        {
            _failure = $"the native API table carries no address for '{name}'";
            return null;
        }
        return Marshal.GetDelegateForFunctionPointer<T>((IntPtr)address);
    }

    private static void Require()
    {
        Load();
        if (_failure != null)
        {
            throw new InvalidOperationException("Networked: " + _failure);
        }
    }

    public static IntPtr MethodBind(
        string className, string methodName, ulong hash)
    {
        Require();
        IntPtr bind = _methodBind(className, methodName, hash);
        if (bind == IntPtr.Zero)
        {
            throw new MissingMethodException(
                $"Networked: {className}.{methodName} is not bound with hash " +
                $"{hash}. The generated bindings are older or newer than the " +
                "installed addon.");
        }
        return bind;
    }

    /// <summary>
    /// One fixed-arity entry point from the table, bound on first use.
    /// </summary>
    internal static T Thunk<T>(string name) where T : Delegate
    {
        Require();
        T found = Bind<T>(_functions, name);
        if (found == null)
        {
            throw new InvalidOperationException("Networked: " + _failure);
        }
        return found;
    }

    /// <summary>
    /// Takes a reference on a reference-counted object and answers it back, so
    /// a handle adopted from a call outlives the value the call answered.
    /// </summary>
    public static IntPtr Retained(IntPtr instance)
    {
        if (instance != IntPtr.Zero)
        {
            Require();
            _retain(instance);
        }
        return instance;
    }

    internal static IntPtr Construct(string className, bool counted)
    {
        if (!ClassDB.CanInstantiate(className))
        {
            throw new InvalidOperationException(
                $"Cannot construct {className}. Use its declaration or " +
                "session method to obtain an instance.");
        }
        using Variant value = ClassDB.Instantiate(className);
        IntPtr native = ObjectOf(value);
        if (native == IntPtr.Zero)
        {
            throw new InvalidOperationException(
                $"Cannot construct {className}. Load the matching " +
                "Networked extension before constructing it.");
        }
        return counted ? Retained(native) : native;
    }

    public static void Release(IntPtr instance)
    {
        _release(instance);
    }

    /// <summary>
    /// The object a Variant holds, or zero when it holds none.
    /// </summary>
    public static IntPtr ObjectOf(Variant value)
    {
        godot_variant held = value.CopyNativeVariant();
        IntPtr found = VariantUtils.ConvertToGodotObjectPtr(held);
        held.Dispose();
        return found;
    }

    /// <summary>
    /// Every element of an array the addon answered, wrapped as
    /// <typeparamref name="T"/>. A null element stays null.
    /// </summary>
    public static T[] Elements<T>(
        Godot.Collections.Array values,
        Func<Variant, T> wrap
    ) where T : class
    {
        var wrapped = new T[values.Count];
        for (int at = 0; at < values.Count; at++)
        {
            Variant element = values[at];
            wrapped[at] = element.VariantType == Variant.Type.Nil
                ? null
                : wrap(element);
        }
        return wrapped;
    }

    /// <summary>
    /// An array the addon accepts, holding the objects of
    /// <paramref name="values"/>.
    /// </summary>
    public static Godot.Collections.Array ArrayOf(NetwObject[] values)
    {
        var held = new Godot.Collections.Array();
        if (values == null)
        {
            return held;
        }
        foreach (NetwObject value in values)
        {
            held.Add(Variant.CreateTakingOwnershipOfDisposableValue(
                VariantUtils.CreateFromGodotObjectPtr(
                    value?.Native ?? IntPtr.Zero)));
        }
        return held;
    }

    private static readonly ConcurrentDictionary<(Type, string), bool>
        Overloaded = new();

    /// <summary>
    /// The node a method group was taken from, which Godot then calls by
    /// the name of <paramref name="method"/>. Throws when that name would
    /// not reach the method the caller wrote.
    /// </summary>
    public static Node Receiver(
        Delegate method,
        string verb,
        bool registered = true)
    {
        string name = method.Method.Name;
        if (method.Target is not Node node)
        {
            throw new ArgumentException(
                $"{verb} was given '{name}', which is not a method " +
                "of a Node. Pass a method of a Node.",
                nameof(method));
        }
        Type type = node.GetType();
        if (Overloaded.GetOrAdd((type, name), CountsMany))
        {
            throw new ArgumentException(
                $"{verb} was given '{name}', which {type.Name} " +
                "declares more than once. Godot calls a method by name. " +
                "Rename one of them.",
                nameof(method));
        }
        if (registered && !node.HasMethod(name))
        {
            throw new ArgumentException(
                $"{verb} was given '{name}', which Godot cannot " +
                $"call on {type.Name}. Pass a named method of the node " +
                "whose parameters Godot can convert.",
                nameof(method));
        }
        return node;
    }

    /// <summary>
    /// Logs an exception a handler threw while the addon was calling it.
    /// The addon then reads the call as one that gave no answer.
    /// </summary>
    public static void Report(string site, Exception thrown)
    {
        GD.PushError(
            $"The handler given to {site} threw " +
            $"{thrown.GetType().Name}. {thrown.Message}\n{thrown}");
    }

    /// <summary>
    /// Makes an object of the addon class <paramref name="className"/> that
    /// runs the script of <typeparamref name="T"/>, and returns it. The
    /// addon calls a method only by the exact name in
    /// <paramref name="virtuals"/>, so a method that would never be called
    /// throws.
    /// </summary>
    public static T Create<T>(string className, params string[] virtuals)
        where T : GodotObject
    {
        const BindingFlags Declared = BindingFlags.Instance
            | BindingFlags.Public
            | BindingFlags.NonPublic;
        Type type = typeof(T);
        string path = type.GetCustomAttribute<ScriptPathAttribute>(false)?.Path;
        if (path == null)
        {
            throw new ArgumentException(
                $"{className}.Create was given {type.Name}, which is not a " +
                "script. Declare it as a partial class in a file of its " +
                "own name.");
        }
        foreach (MethodInfo one in type.GetMethods(Declared))
        {
            if (!one.Name.StartsWith('_'))
            {
                continue;
            }
            string plain = one.Name.Replace("_", "").ToLowerInvariant();
            foreach (string name in virtuals)
            {
                if (one.Name != name && plain == name.Replace("_", ""))
                {
                    throw new ArgumentException(
                        $"{type.Name} declares '{one.Name}', which " +
                        $"{className} never calls. Name the method " +
                        $"'{name}'.");
                }
            }
        }
        Variant held = ClassDB.Instantiate(className);
        ulong id = held.AsGodotObject().GetInstanceId();
        held.AsGodotObject().SetScript(GD.Load<CSharpScript>(path));
        GodotObject live = GodotObject.InstanceFromId(id);
        string refused = null;
        if (live is not T)
        {
            refused = $"{className}.Create was given {type.Name}, which " +
                $"cannot be attached to a {className}. Derive it from " +
                $"{live.GetType().Name}.";
        }
        foreach (string name in virtuals)
        {
            if (refused == null
                && type.GetMember(name, MemberTypes.Method, Declared).Length > 0
                && !live.HasMethod(name))
            {
                refused = $"{type.Name} declares '{name}' with a type " +
                    "Godot cannot pass. Type an object of the addon as " +
                    "RefCounted or Node and read it with From.";
            }
        }
        if (refused != null)
        {
            (live as Node)?.Free();
            throw new ArgumentException(refused);
        }
        return (T)live;
    }

    /// <summary>
    /// The promise the addon waits for when a handler awaits. It completes
    /// when <paramref name="task"/> does. A failure is logged and fails
    /// the promise.
    /// </summary>
    public static Variant Awaited(string site, Task task)
    {
        var promise = new NetwPromise();
        Watch(site, task, promise);
        return promise;
    }

    private static async void Watch(
        string site,
        Task task,
        NetwPromise promise)
    {
        try
        {
            await task;
        }
        catch (Exception thrown)
        {
            Report(site, thrown);
            NetwSettle.Fail(promise, thrown);
            return;
        }
        promise.Resolve(default);
    }

    /// <summary>
    /// <paramref name="bridge"/>, made to report the object
    /// <paramref name="handler"/> belongs to as its own. Godot reads a
    /// callable's object from the last delegate of a chain, so the chain
    /// ends in <paramref name="anchor"/>, which does nothing.
    /// </summary>
    public static T Anchored<T>(
        Delegate handler,
        T bridge,
        Func<GodotObject, T> anchor) where T : Delegate
    {
        return handler.Target is GodotObject owner
            ? (T)Delegate.Combine(bridge, anchor(owner))
            : bridge;
    }

    [ThreadStatic]
    private static Variant _answer;

    /// <summary>
    /// Keeps what a handler returned until the anchor of its chain hands
    /// it to the addon.
    /// </summary>
    public static Variant Answered(Variant answer)
    {
        _answer = answer;
        return answer;
    }

    private static Variant Taken()
    {
        Variant answer = _answer;
        _answer = default;
        return answer;
    }

#pragma warning disable IDE0060
    public static void NetwHold(this GodotObject o) { }
    public static void NetwHold(this GodotObject o, Variant a) { }
    public static void NetwHold(this GodotObject o, Variant a, Variant b) { }
    public static void NetwHold(
        this GodotObject o, Variant a, Variant b, Variant c) { }
    public static void NetwHold(
        this GodotObject o, Variant a, Variant b, Variant c, Variant d) { }
    public static void NetwHold(
        this GodotObject o, Variant a, Variant b, Variant c, Variant d,
        Variant e) { }
    public static void NetwHold(
        this GodotObject o, Variant a, Variant b, Variant c, Variant d,
        Variant e, Variant f) { }
    public static void NetwHold(
        this GodotObject o, Variant a, Variant b, Variant c, Variant d,
        Variant e, Variant f, Variant g) { }
    public static void NetwHold(
        this GodotObject o, Variant a, Variant b, Variant c, Variant d,
        Variant e, Variant f, Variant g, Variant h) { }
    public static void NetwHold(
        this GodotObject o, Variant a, Variant b, Variant c, Variant d,
        Variant e, Variant f, Variant g, Variant h, Variant i) { }
    public static Variant NetwAnswer(this GodotObject o) => Taken();
    public static Variant NetwAnswer(this GodotObject o, Variant a) =>
        Taken();
    public static Variant NetwAnswer(
        this GodotObject o, Variant a, Variant b) => Taken();
    public static Variant NetwAnswer(
        this GodotObject o, Variant a, Variant b, Variant c) => Taken();
#pragma warning restore IDE0060

    private static bool CountsMany((Type, string) key)
    {
        const BindingFlags Declared = BindingFlags.Instance
            | BindingFlags.Static
            | BindingFlags.Public
            | BindingFlags.NonPublic
            | BindingFlags.DeclaredOnly;
        int count = 0;
        for (Type at = key.Item1; at != null; at = at.BaseType)
        {
            foreach (MethodInfo one in at.GetMethods(Declared))
            {
                if (one.Name == key.Item2
                    && one.GetBaseDefinition() == one)
                {
                    count++;
                }
            }
        }
        return count > 1;
    }
}
