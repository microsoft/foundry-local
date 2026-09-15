namespace ModelManagement.Interfaces
{
    public interface IEPManagement
    {
        public Task DownloadAndRegisterEpsAsync(CancellationToken ct = default);
    }
}