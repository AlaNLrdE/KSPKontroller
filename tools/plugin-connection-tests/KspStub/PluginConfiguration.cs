// Only the settings static initializer is used; no game scene or Unity native API is invoked.
public class Vessel {}
namespace KSP.IO
{
    public class PluginConfiguration
    {
        public static PluginConfiguration CreateForType<T>(Vessel flight = null) { return new PluginConfiguration(); }
    }
}
