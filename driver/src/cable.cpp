/*++

Module Name:

    cable.cpp

Abstract:

    Реализация кольцевого буфера виртуального кабеля. См. cable.h.

--*/

#include "definitions.h"
#include "cable.h"

#define CABLE_POOLTAG 'lbCB'

CCableBuffer g_Cable;

#pragma code_seg("PAGE")
NTSTATUS CCableBuffer::Init(_In_ ULONG capacityBytes, _In_ ULONG primeBytes)
{
    PAGED_CODE();

    KeInitializeSpinLock(&m_lock);
    m_buffer = (BYTE*)ExAllocatePool2(POOL_FLAG_NON_PAGED, capacityBytes, CABLE_POOLTAG);
    if (m_buffer == NULL)
    {
        m_capacity = 0;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    m_capacity = capacityBytes;
    m_prime = primeBytes < capacityBytes ? primeBytes : capacityBytes / 2;
    m_head = 0;
    m_count = 0;
    m_primed = FALSE;
    m_underruns = 0;
    m_overruns = 0;
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
void CCableBuffer::Free()
{
    PAGED_CODE();

    if (m_buffer != NULL)
    {
        ExFreePoolWithTag(m_buffer, CABLE_POOLTAG);
        m_buffer = NULL;
    }
    m_capacity = 0;
    m_count = 0;
}

#pragma code_seg()
void CCableBuffer::Write(_In_reads_bytes_(bytes) const BYTE* data, _In_ ULONG bytes)
{
    if (m_buffer == NULL || bytes == 0)
    {
        return;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_lock, &oldIrql);

    if (bytes > m_capacity)
    {
        // Держим только хвост.
        data += bytes - m_capacity;
        bytes = m_capacity;
    }

    ULONG first = m_capacity - m_head;
    if (first > bytes)
    {
        first = bytes;
    }
    RtlCopyMemory(m_buffer + m_head, data, first);
    if (bytes > first)
    {
        RtlCopyMemory(m_buffer, data + first, bytes - first);
    }
    m_head = (m_head + bytes) % m_capacity;

    if (m_count + bytes > m_capacity)
    {
        // Переполнение: самые старые данные затёрты.
        m_overruns++;
        m_count = m_capacity;
    }
    else
    {
        m_count += bytes;
    }

    KeReleaseSpinLock(&m_lock, oldIrql);
}

#pragma code_seg()
void CCableBuffer::Read(_Out_writes_bytes_all_(bytes) BYTE* data, _In_ ULONG bytes)
{
    if (bytes == 0)
    {
        return;
    }
    if (m_buffer == NULL)
    {
        RtlZeroMemory(data, bytes);
        return;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_lock, &oldIrql);

    if (!m_primed)
    {
        if (m_count >= m_prime)
        {
            m_primed = TRUE;
        }
        else
        {
            KeReleaseSpinLock(&m_lock, oldIrql);
            RtlZeroMemory(data, bytes);
            return;
        }
    }

    ULONG n = bytes < m_count ? bytes : m_count;
    ULONG tail = (m_head + m_capacity - m_count) % m_capacity;
    ULONG first = m_capacity - tail;
    if (first > n)
    {
        first = n;
    }
    RtlCopyMemory(data, m_buffer + tail, first);
    if (n > first)
    {
        RtlCopyMemory(data + first, m_buffer, n - first);
    }
    m_count -= n;

    if (n < bytes)
    {
        // Опустошение: тишина до конца запроса, дальше снова ждём накопления.
        RtlZeroMemory(data + n, bytes - n);
        m_primed = FALSE;
        m_underruns++;
    }

    KeReleaseSpinLock(&m_lock, oldIrql);
}
