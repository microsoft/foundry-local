# Configuration

Namespace: Microsoft.AI.Foundry.Local

```csharp
public class Configuration
```

Inheritance [Object](https://docs.microsoft.com/en-us/dotnet/api/system.object) → [Configuration](./microsoft.ai.foundry.local.configuration.md)<br>
Attributes [NullableContextAttribute](https://docs.microsoft.com/en-us/dotnet/api/system.runtime.compilerservices.nullablecontextattribute), [NullableAttribute](https://docs.microsoft.com/en-us/dotnet/api/system.runtime.compilerservices.nullableattribute), [RequiredMemberAttribute](https://docs.microsoft.com/en-us/dotnet/api/system.runtime.compilerservices.requiredmemberattribute)

## Properties

### **AppName**

Your application name. MUST be set to a valid name.

```csharp
public string AppName { get; set; }
```

#### Property Value

[String](https://docs.microsoft.com/en-us/dotnet/api/system.string)<br>

### **AppDataDir**

Application data directory.
 Default: {home}/.{appname}, where {home} is the user's home directory and {appname} is the AppName value.

```csharp
public string AppDataDir { get; set; }
```

#### Property Value

[String](https://docs.microsoft.com/en-us/dotnet/api/system.string)<br>

### **ModelCacheDir**

Model cache directory.
 Default: {appdata}/cache/models, where {appdata} is the AppDataDir value.

```csharp
public string ModelCacheDir { get; set; }
```

#### Property Value

[String](https://docs.microsoft.com/en-us/dotnet/api/system.string)<br>

### **LogsDir**

Log directory.
 Default: {appdata}/logs

```csharp
public string LogsDir { get; set; }
```

#### Property Value

[String](https://docs.microsoft.com/en-us/dotnet/api/system.string)<br>

### **LogLevel**

Logging level.
 Valid values are: Verbose, Debug, Information, Warning, Error, Fatal.
 Default: LogLevel.Warning

```csharp
public LogLevel LogLevel { get; set; }
```

#### Property Value

[LogLevel](./microsoft.ai.foundry.local.loglevel.md)<br>

### **Web**

Optional configuration for the built-in web service.
 NOTE: This is not included in all builds.

```csharp
public WebService Web { get; set; }
```

#### Property Value

[WebService](./microsoft.ai.foundry.local.configuration.webservice.md)<br>

### **UseSharedHost**

Route existing model/session APIs through the packaged native shared host. Defaults to `false` (in-process).
Execution or web-service Start lazily starts or joins a compatible host; discovery does not start it.
Manager disposal releases only this client. Cannot be combined with an external web-service URL.

```csharp
public bool UseSharedHost { get; init; }
```

#### Property Value

[Boolean](https://docs.microsoft.com/en-us/dotnet/api/system.boolean)<br>

### **AdditionalSettings**

Additional settings that Foundry Local Core can consume.
 Keys and values are strings.

```csharp
public IDictionary<string, string> AdditionalSettings { get; set; }
```

#### Property Value

[IDictionary&lt;String, String&gt;](https://docs.microsoft.com/en-us/dotnet/api/system.collections.generic.idictionary-2)<br>

## Constructors

### **Configuration()**

#### Caution

Constructors of types with required members are not supported in this version of your compiler.

---

```csharp
public Configuration()
```
