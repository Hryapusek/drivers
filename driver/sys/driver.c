#include <ntddk.h>
#include <wdf.h>

#define IOCTL_SEND_DATA_TO_PORT CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#define MAX_BUFFER_SIZE 256
#define COM2_PORT_BASE_ADDRESS 0x2F8 // Example base address for COM2

VOID EvtIoWrite(
    __in PDEVICE_OBJECT DeviceObject,
    __in PIRP Irp,
    __in size_t Length
);

typedef struct _DEVICE_EXTENSION {
    CHAR DataBuffer[MAX_BUFFER_SIZE];
    ULONG DataLength;
} DEVICE_EXTENSION, *PDEVICE_EXTENSION;

// Function to configure the serial port
NTSTATUS ConfigureSerialPort(USHORT PortBase)
{
    UCHAR divisor;
    NTSTATUS status;

    // Reset the COM port (disable interrupts)
    WRITE_PORT_UCHAR(PortBase + 4, 0x00);  // Disable interrupts (IER)

    // Set the baud rate - Divisor Latch Access Bit (DLAB = 1)
    WRITE_PORT_UCHAR(PortBase + 3, 0x80);  // Enable DLAB
    divisor = 1; // Example divisor for 115600 baud rate
    WRITE_PORT_UCHAR(PortBase, divisor);   // Set LSB of divisor
    WRITE_PORT_UCHAR(PortBase + 1, divisor); // Set MSB of divisor

    // Set 8 data bits, no parity, 1 stop bit
    WRITE_PORT_UCHAR(PortBase + 3, 0x03);  // 8 data bits, no parity, 1 stop bit

    // Enable FIFO for serial port
    WRITE_PORT_UCHAR(PortBase + 2, 0xC7);  // Enable FIFO, clear them, 14-byte threshold

    // Enable modem control
    WRITE_PORT_UCHAR(PortBase + 4, 0x0F);  // Enable both receiver and transmitter

    DbgPrint("COM2 port configured successfully.\n");

    return STATUS_SUCCESS;
}

// Check if the transmitter is ready
BOOLEAN IsTransmitterReady()
{
    UCHAR lsr;

    // Read the Line Status Register (LSR) from COM2 (PortBase + 5)
    lsr = READ_PORT_UCHAR(COM2_PORT_BASE_ADDRESS + 5);  // LSR is at offset 5

    // Check if the Transmitter Holding Register Empty (THRE) bit is set (bit 5)
    DbgPrint("LSR Register: 0x%x\n", lsr);
    return (lsr & 0x20) != 0;
}

// Wait for the transmitter to be ready to send data
NTSTATUS WaitForTransmitterReady()
{
    LARGE_INTEGER delayTime;
    NTSTATUS status;

    // Set a timeout for checking (e.g., 100 milliseconds)
    delayTime.QuadPart = -10000 * 100;  // 100ms in 100-nanosecond units

    DbgPrint("Waiting for transmitter to be ready...\n");

    while (!IsTransmitterReady()) {
        // Wait for the timeout period before checking again
        KeDelayExecutionThread(KernelMode, FALSE, &delayTime);
    }

    DbgPrint("Transmitter is ready.\n");
    return STATUS_SUCCESS;
}

// Write data to the serial port
NTSTATUS CustomIoWrite(
    __in PDEVICE_OBJECT DeviceObject,
    __in PIRP Irp,
    __in size_t Length
)
{
    PDEVICE_EXTENSION deviceExtension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;
    NTSTATUS status = STATUS_SUCCESS;
    size_t bytesWritten = 0;
    PVOID buffer = NULL;

    DbgPrint("CustomIoWrite: Start writing to port\n");

    // Check that Length is valid and non-zero
    if (Length == 0) {
        DbgPrint("CustomIoWrite: Invalid length 0\n");
        return STATUS_INVALID_PARAMETER;
    }

    // Retrieve the input buffer from the IRP (this is the user data sent to the driver)
    buffer = Irp->AssociatedIrp.SystemBuffer;
    if (buffer == NULL) {
        DbgPrint("CustomIoWrite: Buffer is NULL\n");
        return STATUS_INVALID_PARAMETER;
    }

    // Start sending bytes one by one to the serial port
    for (bytesWritten = 0; bytesWritten < Length; bytesWritten++) {
        DbgPrint("CustomIoWrite: Writing byte %zu: 0x%02X\n", bytesWritten, ((PUCHAR)buffer)[bytesWritten]);

        // Wait for the transmitter to be ready
        status = WaitForTransmitterReady();
        if (!NT_SUCCESS(status)) {
            DbgPrint("CustomIoWrite: Transmitter not ready. Status: 0x%x\n", status);
            return status;
        }

        // Send the byte to the serial port (COM2 for example)
        WRITE_PORT_UCHAR(COM2_PORT_BASE_ADDRESS, ((PUCHAR)buffer)[bytesWritten]);
        DbgPrint("CustomIoWrite: Sent byte 0x%x to COM2\n", ((PUCHAR)buffer)[bytesWritten]);
    }

    // Complete the IRP after all bytes are written
    DbgPrint("CustomIoWrite: Completed writing to serial port\n");
}


// IOCTL handler for sending data to serial port
NTSTATUS DeviceIoControlHandler(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
)
{
    PDEVICE_EXTENSION deviceExtension = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION ioStack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = STATUS_SUCCESS;
    ULONG bytesToCopy = 0;

    // Check if the correct IOCTL code was used
    if (ioStack->Parameters.DeviceIoControl.IoControlCode == IOCTL_SEND_DATA_TO_PORT) {
        bytesToCopy = min(ioStack->Parameters.DeviceIoControl.InputBufferLength, MAX_BUFFER_SIZE);

        // Copy the input buffer to the device extension data buffer
        RtlCopyMemory(deviceExtension->DataBuffer, Irp->AssociatedIrp.SystemBuffer, bytesToCopy);
        deviceExtension->DataLength = bytesToCopy;

        // Debug: Print received data and length
        DbgPrint("DeviceIoControlHandler: Received %lu bytes, data: %.*s\n", 
            bytesToCopy, bytesToCopy, deviceExtension->DataBuffer);

        // Pass data to the serial port write function
        status = CustomIoWrite(DeviceObject, Irp, bytesToCopy);
    } else {
        DbgPrint("DeviceIoControlHandler: Unknown IOCTL code\n");
    }

    Irp->IoStatus.Information = bytesToCopy;
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT); // Complete the IRP request

    return STATUS_SUCCESS;
}

// Create or close the device (dummy handler)
NTSTATUS CreateCloseHandler(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
)
{
    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

// DriverEntry function (entry point)
NTSTATUS DriverEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PUNICODE_STRING RegistryPath
)
{
    NTSTATUS status;
    UNICODE_STRING deviceName;
    UNICODE_STRING symbolicLinkName;
    WDFDEVICE device;
    PDEVICE_EXTENSION deviceExtension;

    // Initialize the device and symbolic link
    RtlInitUnicodeString(&deviceName, L"\\Device\\IoctlDevice");
    RtlInitUnicodeString(&symbolicLinkName, L"\\DosDevices\\IoctlDevice");

    // Create the device object
    status = IoCreateDevice(
        DriverObject,
        sizeof(DEVICE_EXTENSION),
        &deviceName,
        FILE_DEVICE_UNKNOWN,
        0,
        FALSE,
        &device
    );

    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Configure the serial port (COM2)
    status = ConfigureSerialPort(COM2_PORT_BASE_ADDRESS);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(device);
        return status;
    }

    // Initialize device extension
    deviceExtension = (PDEVICE_EXTENSION)DriverObject->DeviceObject->DeviceExtension;
    deviceExtension->DataLength = 0;

    // Set up major function handlers
    DriverObject->MajorFunction[IRP_MJ_CREATE] = CreateCloseHandler;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = CreateCloseHandler;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DeviceIoControlHandler;

    // Create the symbolic link for user-mode access
    status = IoCreateSymbolicLink(&symbolicLinkName, &deviceName);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(device);
        return status;
    }

    return STATUS_SUCCESS;
}
