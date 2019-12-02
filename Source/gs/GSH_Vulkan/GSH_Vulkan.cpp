#include <cstring>
#include "../GsPixelFormats.h"
#include "../../Log.h"
#include "GSH_Vulkan.h"
#include "vulkan/StructDefs.h"
#include "vulkan/Utils.h"

#define LOG_NAME ("gsh_vulkan")

using namespace GSH_Vulkan;

#define MEMORY_WIDTH 1024
#define MEMORY_HEIGHT 1024

static uint32 MakeColor(uint8 r, uint8 g, uint8 b, uint8 a)
{
	return (a << 24) | (b << 16) | (g << 8) | (r);
}

CGSH_Vulkan::CGSH_Vulkan()
{
	m_context = std::make_shared<CContext>();
}

CGSH_Vulkan::~CGSH_Vulkan()
{

}

void CGSH_Vulkan::InitializeImpl()
{
	assert(m_instance != nullptr);

	auto physicalDevices = GetPhysicalDevices();
	assert(!physicalDevices.empty());
	auto physicalDevice = physicalDevices[0];

	auto renderQueueFamilies = GetRenderQueueFamilies(physicalDevice);
	assert(!renderQueueFamilies.empty());
	auto renderQueueFamily = renderQueueFamilies[0];

	m_instance.vkGetPhysicalDeviceMemoryProperties(physicalDevice, &m_context->physicalDeviceMemoryProperties);

	auto surfaceFormats = GetDeviceSurfaceFormats(physicalDevice);
	assert(surfaceFormats.size() > 0);
	m_context->surfaceFormat = surfaceFormats[0];

	{
		VkSurfaceCapabilitiesKHR surfaceCaps = {};
		auto result = m_instance.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, m_context->surface, &surfaceCaps);
		CHECKVULKANERROR(result);
		CLog::GetInstance().Print(LOG_NAME, "Surface Current Extents: %d, %d\r\n", 
			surfaceCaps.currentExtent.width, surfaceCaps.currentExtent.height);
		m_context->surfaceExtents = surfaceCaps.currentExtent;
	}

	CreateDevice(physicalDevice);
	m_context->device.vkGetDeviceQueue(m_context->device, renderQueueFamily, 0, &m_context->queue);
	m_context->commandBufferPool = Framework::Vulkan::CCommandBufferPool(m_context->device, renderQueueFamily);

	CreateDescriptorPool();
	CreateMemoryImage();
	InitMemoryImage();

	m_draw = std::make_shared<CDraw>(m_context);
	m_present = std::make_shared<CPresent>(m_context);
	m_transfer = std::make_shared<CTransfer>(m_context);
}

void CGSH_Vulkan::ReleaseImpl()
{
	ResetImpl();
	
	//Flush any pending rendering commands
	m_context->device.vkQueueWaitIdle(m_context->queue);

	m_draw.reset();
	m_present.reset();
	m_transfer.reset();

	m_context->device.vkDestroyImageView(m_context->device, m_context->memoryImageView, nullptr);
	m_context->device.vkDestroyImage(m_context->device, m_memoryImage, nullptr);
	m_context->device.vkFreeMemory(m_context->device, m_memoryImageMemoryHandle, nullptr);
	m_context->device.vkDestroyDescriptorPool(m_context->device, m_context->descriptorPool, nullptr);
	m_context->commandBufferPool.Reset();
	m_context->device.Reset();
}

void CGSH_Vulkan::ResetImpl()
{
	m_vtxCount = 0;
	m_primitiveType = PRIM_INVALID;
}

void CGSH_Vulkan::FlipImpl()
{
	m_draw->FlushCommands();

	DISPLAY d;
	DISPFB fb;
	{
		std::lock_guard<std::recursive_mutex> registerMutexLock(m_registerMutex);
		unsigned int readCircuit = GetCurrentReadCircuit();
		switch(readCircuit)
		{
		case 0:
			d <<= m_nDISPLAY1.value.q;
			fb <<= m_nDISPFB1.value.q;
			break;
		case 1:
			d <<= m_nDISPLAY2.value.q;
			fb <<= m_nDISPFB2.value.q;
			break;
		}
	}

	m_present->DoPresent(fb.GetBufPtr(), fb.GetBufWidth());

	auto result = m_context->device.vkResetDescriptorPool(m_context->device, m_context->descriptorPool, 0);
	CHECKVULKANERROR(result);

	m_context->commandBufferPool.ResetBuffers();

	PresentBackbuffer();
	CGSHandler::FlipImpl();
}

unsigned int CGSH_Vulkan::GetCurrentReadCircuit()
{
	uint32 rcMode = m_nPMODE & 0x03;
	switch(rcMode)
	{
	default:
	case 0:
		//No read circuit enabled?
		return 0;
	case 1:
		return 0;
	case 2:
		return 1;
	case 3:
	{
		//Both are enabled... See if we can find out which one is good
		//This happens in Capcom Classics Collection Vol. 2
		std::lock_guard<std::recursive_mutex> registerMutexLock(m_registerMutex);
		bool fb1Null = (m_nDISPFB1.value.q == 0);
		bool fb2Null = (m_nDISPFB2.value.q == 0);
		if(!fb1Null && fb2Null)
		{
			return 0;
		}
		if(fb1Null && !fb2Null)
		{
			return 1;
		}
		return 0;
	}
	break;
	}
}

void CGSH_Vulkan::LoadState(Framework::CZipArchiveReader& archive)
{
	CGSHandler::LoadState(archive);
}

void CGSH_Vulkan::NotifyPreferencesChangedImpl()
{
	CGSHandler::NotifyPreferencesChangedImpl();
}

std::vector<VkPhysicalDevice> CGSH_Vulkan::GetPhysicalDevices()
{
	auto result = VK_SUCCESS;

	uint32_t physicalDeviceCount = 0;
	result = m_instance.vkEnumeratePhysicalDevices(m_instance, &physicalDeviceCount, nullptr);
	CHECKVULKANERROR(result);
	
	CLog::GetInstance().Print(LOG_NAME, "Found %d physical devices.\r\n", physicalDeviceCount);

	std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
	result = m_instance.vkEnumeratePhysicalDevices(m_instance, &physicalDeviceCount, physicalDevices.data());
	CHECKVULKANERROR(result);
	
	for(const auto& physicalDevice : physicalDevices)
	{
		CLog::GetInstance().Print(LOG_NAME, "Physical Device Info:\r\n");
		
		VkPhysicalDeviceProperties physicalDeviceProperties = {};
		m_instance.vkGetPhysicalDeviceProperties(physicalDevice, &physicalDeviceProperties);
		CLog::GetInstance().Print(LOG_NAME, "Driver Version: %d\r\n", physicalDeviceProperties.driverVersion);
		CLog::GetInstance().Print(LOG_NAME, "Device Name:    %s\r\n", physicalDeviceProperties.deviceName);
		CLog::GetInstance().Print(LOG_NAME, "Device Type:    %d\r\n", physicalDeviceProperties.deviceType);
		CLog::GetInstance().Print(LOG_NAME, "API Version:    %d.%d.%d\r\n",
			VK_VERSION_MAJOR(physicalDeviceProperties.apiVersion),
			VK_VERSION_MINOR(physicalDeviceProperties.apiVersion),
			VK_VERSION_PATCH(physicalDeviceProperties.apiVersion));
	}
	
	return physicalDevices;
}

std::vector<uint32_t> CGSH_Vulkan::GetRenderQueueFamilies(VkPhysicalDevice physicalDevice)
{
	assert(m_context->surface != VK_NULL_HANDLE);
	
	auto result = VK_SUCCESS;
	std::vector<uint32_t> renderQueueFamilies;
	
	uint32_t queueFamilyCount = 0;
	m_instance.vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
	
	CLog::GetInstance().Print(LOG_NAME, "Found %d queue families.\r\n", queueFamilyCount);
	
	std::vector<VkQueueFamilyProperties> queueFamilyPropertiesArray(queueFamilyCount);
	m_instance.vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilyPropertiesArray.data());
	
	for(uint32_t queueFamilyIndex = 0; queueFamilyIndex < queueFamilyCount; queueFamilyIndex++)
	{
		bool graphicsSupported = false;
		
		CLog::GetInstance().Print(LOG_NAME, "Queue Family Info:\r\n");

		const auto& queueFamilyProperties = queueFamilyPropertiesArray[queueFamilyIndex];
		CLog::GetInstance().Print(LOG_NAME, "Queue Count:    %d\r\n", queueFamilyProperties.queueCount);
		CLog::GetInstance().Print(LOG_NAME, "Operating modes:\r\n");
		if(queueFamilyProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT)
		{
			graphicsSupported = true;
			CLog::GetInstance().Print(LOG_NAME, "  Graphics\r\n");
		}
		if(queueFamilyProperties.queueFlags & VK_QUEUE_COMPUTE_BIT)
		{
			CLog::GetInstance().Print(LOG_NAME, "  Compute\r\n");
		}
		if(queueFamilyProperties.queueFlags & VK_QUEUE_TRANSFER_BIT)
		{
			CLog::GetInstance().Print(LOG_NAME, "  Transfer\r\n");
		}
		if(queueFamilyProperties.queueFlags & VK_QUEUE_SPARSE_BINDING_BIT)
		{
			CLog::GetInstance().Print(LOG_NAME, "  Sparse Binding\r\n");
		}
		
		VkBool32 surfaceSupported = VK_FALSE;
		result = m_instance.vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, queueFamilyIndex, m_context->surface, &surfaceSupported);
		CHECKVULKANERROR(result);
		
		CLog::GetInstance().Print(LOG_NAME, "Supports surface: %d\r\n", surfaceSupported);
		
		if(graphicsSupported && surfaceSupported)
		{
			renderQueueFamilies.push_back(queueFamilyIndex);
		}
	}
	
	return renderQueueFamilies;
}

std::vector<VkSurfaceFormatKHR> CGSH_Vulkan::GetDeviceSurfaceFormats(VkPhysicalDevice physicalDevice)
{
	assert(m_context->surface != VK_NULL_HANDLE);

	auto result = VK_SUCCESS;
	
	uint32_t surfaceFormatCount = 0;
	result = m_instance.vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, m_context->surface, &surfaceFormatCount, nullptr);
	CHECKVULKANERROR(result);
	
	CLog::GetInstance().Print(LOG_NAME, "Found %d surface formats.\r\n", surfaceFormatCount);

	std::vector<VkSurfaceFormatKHR> surfaceFormats(surfaceFormatCount);
	result = m_instance.vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, m_context->surface, &surfaceFormatCount, surfaceFormats.data());
	CHECKVULKANERROR(result);
	
	for(const auto& surfaceFormat : surfaceFormats)
	{
		CLog::GetInstance().Print(LOG_NAME, "Surface Format Info:\r\n");
		
		CLog::GetInstance().Print(LOG_NAME, "Format:      %d\r\n", surfaceFormat.format);
		CLog::GetInstance().Print(LOG_NAME, "Color Space: %d\r\n", surfaceFormat.colorSpace);
	}
	
	return surfaceFormats;
}

void CGSH_Vulkan::CreateDevice(VkPhysicalDevice physicalDevice)
{
	assert(m_context->device.IsEmpty());

	float queuePriorities[] = { 1.0f };
	
	auto deviceQueueCreateInfo = Framework::Vulkan::DeviceQueueCreateInfo();
	deviceQueueCreateInfo.flags            = 0;
	deviceQueueCreateInfo.queueFamilyIndex = 0;
	deviceQueueCreateInfo.queueCount       = 1;
	deviceQueueCreateInfo.pQueuePriorities = queuePriorities;

	std::vector<const char*> enabledExtensions;
	enabledExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
	enabledExtensions.push_back(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);
	
	std::vector<const char*> enabledLayers;

	auto physicalDeviceFeaturesInvocationInterlock = Framework::Vulkan::PhysicalDeviceFragmentShaderInterlockFeaturesEXT();
	physicalDeviceFeaturesInvocationInterlock.fragmentShaderPixelInterlock = VK_TRUE;

	auto physicalDeviceFeatures2 = Framework::Vulkan::PhysicalDeviceFeatures2KHR();
	physicalDeviceFeatures2.pNext = &physicalDeviceFeaturesInvocationInterlock;
	physicalDeviceFeatures2.features.fragmentStoresAndAtomics = VK_TRUE;

	auto deviceCreateInfo = Framework::Vulkan::DeviceCreateInfo();
	deviceCreateInfo.pNext                   = &physicalDeviceFeatures2;
	deviceCreateInfo.flags                   = 0;
	deviceCreateInfo.enabledLayerCount       = enabledLayers.size();
	deviceCreateInfo.ppEnabledLayerNames     = enabledLayers.data();
	deviceCreateInfo.enabledExtensionCount   = enabledExtensions.size();
	deviceCreateInfo.ppEnabledExtensionNames = enabledExtensions.data();
	deviceCreateInfo.queueCreateInfoCount    = 1;
	deviceCreateInfo.pQueueCreateInfos       = &deviceQueueCreateInfo;
	
	m_context->device = Framework::Vulkan::CDevice(m_instance, physicalDevice, deviceCreateInfo);
}

void CGSH_Vulkan::CreateDescriptorPool()
{
	std::vector<VkDescriptorPoolSize> poolSizes;

	{
		VkDescriptorPoolSize poolSize = {};
		poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		poolSize.descriptorCount = 0x800;
		poolSizes.push_back(poolSize);
	}
	
	{
		VkDescriptorPoolSize poolSize = {};
		poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		poolSize.descriptorCount = 0x800;
		poolSizes.push_back(poolSize);
	}

	{
		VkDescriptorPoolSize poolSize = {};
		poolSize.type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		poolSize.descriptorCount = 0x800;
		poolSizes.push_back(poolSize);
	}

	auto descriptorPoolCreateInfo = Framework::Vulkan::DescriptorPoolCreateInfo();
	descriptorPoolCreateInfo.poolSizeCount = poolSizes.size();
	descriptorPoolCreateInfo.pPoolSizes    = poolSizes.data();
	descriptorPoolCreateInfo.maxSets       = 0x1000;
	
	auto result = m_context->device.vkCreateDescriptorPool(m_context->device, &descriptorPoolCreateInfo, nullptr, &m_context->descriptorPool);
	CHECKVULKANERROR(result);
}

void CGSH_Vulkan::CreateMemoryImage()
{
	{
		auto imageCreateInfo = Framework::Vulkan::ImageCreateInfo();
		imageCreateInfo.imageType     = VK_IMAGE_TYPE_2D;
		imageCreateInfo.format        = VK_FORMAT_R32_UINT;
		imageCreateInfo.extent.width  = MEMORY_WIDTH;
		imageCreateInfo.extent.height = MEMORY_HEIGHT;
		imageCreateInfo.extent.depth  = 1;
		imageCreateInfo.mipLevels     = 1;
		imageCreateInfo.arrayLayers   = 1;
		imageCreateInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
		imageCreateInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
		imageCreateInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
		imageCreateInfo.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

		auto result = m_context->device.vkCreateImage(m_context->device, &imageCreateInfo, nullptr, &m_memoryImage);
		CHECKVULKANERROR(result);
	}

	{
		VkMemoryRequirements memoryRequirements = {};
		m_context->device.vkGetImageMemoryRequirements(m_context->device, m_memoryImage, &memoryRequirements);

		auto memoryAllocateInfo = Framework::Vulkan::MemoryAllocateInfo();
		memoryAllocateInfo.allocationSize  = memoryRequirements.size;
		memoryAllocateInfo.memoryTypeIndex = Framework::Vulkan::GetMemoryTypeIndex(
			m_context->physicalDeviceMemoryProperties, memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

		auto result = m_context->device.vkAllocateMemory(m_context->device, &memoryAllocateInfo, nullptr, &m_memoryImageMemoryHandle);
		CHECKVULKANERROR(result);
	}
	
	m_context->device.vkBindImageMemory(m_context->device, m_memoryImage, m_memoryImageMemoryHandle, 0);

	{
		auto imageViewCreateInfo = Framework::Vulkan::ImageViewCreateInfo();
		imageViewCreateInfo.image    = m_memoryImage;
		imageViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		imageViewCreateInfo.format   = VK_FORMAT_R32_UINT;
		imageViewCreateInfo.components = 
		{
			VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, 
			VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A 
		};
		imageViewCreateInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		
		auto result = m_context->device.vkCreateImageView(m_context->device, &imageViewCreateInfo, nullptr, &m_context->memoryImageView);
		CHECKVULKANERROR(result);
	}
}

void CGSH_Vulkan::InitMemoryImage()
{
	VkResult result = VK_SUCCESS;
	
	VkBuffer stagingBufferHandle = VK_NULL_HANDLE;
	VkDeviceMemory stagingBufferMemoryHandle = VK_NULL_HANDLE;
	
	//TODO: Get proper value for that
	uint32 dataSize = MEMORY_WIDTH * MEMORY_HEIGHT * sizeof(uint32);
	
	//Create staging buffer for our texture data
	{
		auto bufferCreateInfo = Framework::Vulkan::BufferCreateInfo();
		bufferCreateInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		bufferCreateInfo.size  = dataSize;
		
		result = m_context->device.vkCreateBuffer(m_context->device, &bufferCreateInfo, nullptr, &stagingBufferHandle);
		CHECKVULKANERROR(result);
	}
	
	//Create staging buffer memory
	{
		VkMemoryRequirements memoryRequirements = {};
		m_context->device.vkGetBufferMemoryRequirements(m_context->device, stagingBufferHandle, &memoryRequirements);
		
		auto memoryAllocateInfo = Framework::Vulkan::MemoryAllocateInfo();
		memoryAllocateInfo.allocationSize  = memoryRequirements.size;
		memoryAllocateInfo.memoryTypeIndex = Framework::Vulkan::GetMemoryTypeIndex(m_context->physicalDeviceMemoryProperties, memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		assert(memoryAllocateInfo.memoryTypeIndex != Framework::Vulkan::VULKAN_MEMORY_TYPE_INVALID);
		
		result = m_context->device.vkAllocateMemory(m_context->device, &memoryAllocateInfo, nullptr, &stagingBufferMemoryHandle);
		CHECKVULKANERROR(result);
	}
	
	m_context->device.vkBindBufferMemory(m_context->device, stagingBufferHandle, stagingBufferMemoryHandle, 0);
	
	//Copy image data in buffer
	{
		uint32* memoryPtr = nullptr;
		result = m_context->device.vkMapMemory(m_context->device, stagingBufferMemoryHandle, 0, dataSize, 0, reinterpret_cast<void**>(&memoryPtr));
		CHECKVULKANERROR(result);
		
		for(unsigned int y = 0; y < MEMORY_HEIGHT; y++)
		{
			for(unsigned int x = 0; x < MEMORY_WIDTH; x++)
			{
				uint8 colX = (x * 0x10) / MEMORY_WIDTH;
				uint8 colY = (y * 0x10) / MEMORY_HEIGHT;
				(*memoryPtr) = (static_cast<uint32>(colX) << 12) | (static_cast<uint32>(colY) << 20);
				memoryPtr++;
			}
		}
		
		m_context->device.vkUnmapMemory(m_context->device, stagingBufferMemoryHandle);
	}
	
	auto commandBuffer = m_context->commandBufferPool.AllocateBuffer();
	
	//Start command buffer
	{
		auto commandBufferBeginInfo = Framework::Vulkan::CommandBufferBeginInfo();
		commandBufferBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		
		result = m_context->device.vkBeginCommandBuffer(commandBuffer, &commandBufferBeginInfo);
		CHECKVULKANERROR(result);
	}
	
	//Transition image from whatever state to TRANSFER_DST_OPTIMAL
	{
		auto imageMemoryBarrier = Framework::Vulkan::ImageMemoryBarrier();
		imageMemoryBarrier.image               = m_memoryImage;
		imageMemoryBarrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
		imageMemoryBarrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		imageMemoryBarrier.srcAccessMask       = 0;
		imageMemoryBarrier.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
		imageMemoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		imageMemoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		imageMemoryBarrier.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		
		m_context->device.vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			0, 0, nullptr, 0, nullptr, 1, &imageMemoryBarrier);
	}
	
	//CopyBufferToImage
	{
		VkBufferImageCopy bufferImageCopy = {};
		bufferImageCopy.bufferRowLength    = MEMORY_WIDTH;
		bufferImageCopy.imageSubresource   = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		bufferImageCopy.imageExtent.width  = MEMORY_WIDTH;
		bufferImageCopy.imageExtent.height = MEMORY_HEIGHT;
		bufferImageCopy.imageExtent.depth  = 1;
		
		m_context->device.vkCmdCopyBufferToImage(commandBuffer, stagingBufferHandle, m_memoryImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &bufferImageCopy);
	}
	
	//Transition image from TRANSFER_DST_OPTIMAL to SHADER_READ_ONLY_OPTIMAL
	{
		auto imageMemoryBarrier = Framework::Vulkan::ImageMemoryBarrier();
		imageMemoryBarrier.image               = m_memoryImage;
		imageMemoryBarrier.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		imageMemoryBarrier.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
		imageMemoryBarrier.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
		imageMemoryBarrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
		imageMemoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		imageMemoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		imageMemoryBarrier.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		
		m_context->device.vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			0, 0, nullptr, 0, nullptr, 1, &imageMemoryBarrier);
	}
	
	//Finish command buffer
	result = m_context->device.vkEndCommandBuffer(commandBuffer);
	CHECKVULKANERROR(result);
	
	//Submit command buffer
	{
		auto submitInfo = Framework::Vulkan::SubmitInfo();
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers    = &commandBuffer;
		
		result = m_context->device.vkQueueSubmit(m_context->queue, 1, &submitInfo, VK_NULL_HANDLE);
		CHECKVULKANERROR(result);
	}
	
	//Wait for queue ops to complete
	result = m_context->device.vkQueueWaitIdle(m_context->queue);
	CHECKVULKANERROR(result);
	
	//Destroy staging buffer and memory
	m_context->device.vkFreeMemory(m_context->device, stagingBufferMemoryHandle, nullptr);
	m_context->device.vkDestroyBuffer(m_context->device, stagingBufferHandle, nullptr);
}

void CGSH_Vulkan::VertexKick(uint8 registerId, uint64 data)
{
	if(m_vtxCount == 0) return;

	bool drawingKick = (registerId == GS_REG_XYZ2) || (registerId == GS_REG_XYZF2);
	bool fog = (registerId == GS_REG_XYZF2) || (registerId == GS_REG_XYZF3);

	if(!m_drawEnabled) drawingKick = false;

	if(fog)
	{
		m_vtxBuffer[m_vtxCount - 1].position = data & 0x00FFFFFFFFFFFFFFULL;
		m_vtxBuffer[m_vtxCount - 1].rgbaq = m_nReg[GS_REG_RGBAQ];
		m_vtxBuffer[m_vtxCount - 1].uv = m_nReg[GS_REG_UV];
		m_vtxBuffer[m_vtxCount - 1].st = m_nReg[GS_REG_ST];
		m_vtxBuffer[m_vtxCount - 1].fog = static_cast<uint8>(data >> 56);
	}
	else
	{
		m_vtxBuffer[m_vtxCount - 1].position = data;
		m_vtxBuffer[m_vtxCount - 1].rgbaq = m_nReg[GS_REG_RGBAQ];
		m_vtxBuffer[m_vtxCount - 1].uv = m_nReg[GS_REG_UV];
		m_vtxBuffer[m_vtxCount - 1].st = m_nReg[GS_REG_ST];
		m_vtxBuffer[m_vtxCount - 1].fog = static_cast<uint8>(m_nReg[GS_REG_FOG] >> 56);
	}

	m_vtxCount--;

	if(m_vtxCount == 0)
	{
		if((m_nReg[GS_REG_PRMODECONT] & 1) != 0)
		{
			m_primitiveMode <<= m_nReg[GS_REG_PRIM];
		}
		else
		{
			m_primitiveMode <<= m_nReg[GS_REG_PRMODE];
		}

		if(drawingKick)
		{
			SetRenderingContext(m_primitiveMode);
		}

		switch(m_primitiveType)
		{
#if 0
		case PRIM_POINT:
			if(nDrawingKick) Prim_Point();
			m_nVtxCount = 1;
			break;
		case PRIM_LINE:
			if(nDrawingKick) Prim_Line();
			m_nVtxCount = 2;
			break;
		case PRIM_LINESTRIP:
			if(nDrawingKick) Prim_Line();
			memcpy(&m_VtxBuffer[1], &m_VtxBuffer[0], sizeof(VERTEX));
			m_nVtxCount = 1;
			break;
#endif
		case PRIM_TRIANGLE:
			if(drawingKick) Prim_Triangle();
			m_vtxCount = 3;
			break;
		case PRIM_TRIANGLESTRIP:
			if(drawingKick) Prim_Triangle();
			memcpy(&m_vtxBuffer[2], &m_vtxBuffer[1], sizeof(VERTEX));
			memcpy(&m_vtxBuffer[1], &m_vtxBuffer[0], sizeof(VERTEX));
			m_vtxCount = 1;
			break;
		case PRIM_TRIANGLEFAN:
			if(drawingKick) Prim_Triangle();
			memcpy(&m_vtxBuffer[1], &m_vtxBuffer[0], sizeof(VERTEX));
			m_vtxCount = 1;
			break;
		case PRIM_SPRITE:
			if(drawingKick) Prim_Sprite();
			m_vtxCount = 2;
			break;
		}
	}
}

void CGSH_Vulkan::SetRenderingContext(uint64 primReg)
{
	auto prim = make_convertible<PRMODE>(primReg);

	unsigned int context = prim.nContext;

	auto offset = make_convertible<XYOFFSET>(m_nReg[GS_REG_XYOFFSET_1 + context]);
	auto frame = make_convertible<FRAME>(m_nReg[GS_REG_FRAME_1 + context]);
	auto tex0 = make_convertible<TEX0>(m_nReg[GS_REG_TEX0_1 + context]);

	auto pipelineCaps = make_convertible<CDraw::PIPELINE_CAPS>(0);
	pipelineCaps.hasTexture = prim.nTexture;
	pipelineCaps.textureFormat = tex0.nPsm;

	m_draw->SetPipelineCaps(pipelineCaps);
	m_draw->SetFramebufferBufferInfo(frame.GetBasePtr(), frame.GetWidth());
	m_draw->SetTextureParams(tex0.GetBufPtr(), tex0.GetBufWidth(), 
		tex0.GetWidth(), tex0.GetHeight());

	m_primOfsX = offset.GetX();
	m_primOfsY = offset.GetY();

	m_texWidth = tex0.GetWidth();
	m_texHeight = tex0.GetHeight();
}

void CGSH_Vulkan::Prim_Triangle()
{
	float f1 = 0, f2 = 0, f3 = 0;

	XYZ pos[3];
	pos[0] <<= m_vtxBuffer[2].position;
	pos[1] <<= m_vtxBuffer[1].position;
	pos[2] <<= m_vtxBuffer[0].position;

	float x1 = pos[0].GetX(), x2 = pos[1].GetX(), x3 = pos[2].GetX();
	float y1 = pos[0].GetY(), y2 = pos[1].GetY(), y3 = pos[2].GetY();
	uint32 z1 = pos[0].nZ,    z2 = pos[1].nZ,     z3 = pos[2].nZ;

	RGBAQ rgbaq[3];
	rgbaq[0] <<= m_vtxBuffer[2].rgbaq;
	rgbaq[1] <<= m_vtxBuffer[1].rgbaq;
	rgbaq[2] <<= m_vtxBuffer[0].rgbaq;

	x1 -= m_primOfsX;
	x2 -= m_primOfsX;
	x3 -= m_primOfsX;

	y1 -= m_primOfsY;
	y2 -= m_primOfsY;
	y3 -= m_primOfsY;

	float s[3] = {0, 0, 0};
	float t[3] = {0, 0, 0};
	float q[3] = {1, 1, 1};

	if(m_primitiveMode.nTexture)
	{
		if(m_primitiveMode.nUseUV)
		{
			UV uv[3];
			uv[0] <<= m_vtxBuffer[2].uv;
			uv[1] <<= m_vtxBuffer[1].uv;
			uv[2] <<= m_vtxBuffer[0].uv;

			s[0] = uv[0].GetU() / static_cast<float>(m_texWidth);
			s[1] = uv[1].GetU() / static_cast<float>(m_texWidth);
			s[2] = uv[2].GetU() / static_cast<float>(m_texWidth);

			t[0] = uv[0].GetV() / static_cast<float>(m_texHeight);
			t[1] = uv[1].GetV() / static_cast<float>(m_texHeight);
			t[2] = uv[2].GetV() / static_cast<float>(m_texHeight);
		}
		else
		{
			ST st[3];
			st[0] <<= m_vtxBuffer[2].st;
			st[1] <<= m_vtxBuffer[1].st;
			st[2] <<= m_vtxBuffer[0].st;

			s[0] = st[0].nS;
			s[1] = st[1].nS;
			s[2] = st[2].nS;
			t[0] = st[0].nT;
			t[1] = st[1].nT;
			t[2] = st[2].nT;

			q[0] = rgbaq[0].nQ;
			q[1] = rgbaq[1].nQ;
			q[2] = rgbaq[2].nQ;
		}
	}

	auto color1 = MakeColor(
	    rgbaq[0].nR, rgbaq[0].nG,
	    rgbaq[0].nB, rgbaq[0].nA);

	auto color2 = MakeColor(
	    rgbaq[1].nR, rgbaq[1].nG,
	    rgbaq[1].nB, rgbaq[1].nA);

	auto color3 = MakeColor(
	    rgbaq[2].nR, rgbaq[2].nG,
	    rgbaq[2].nB, rgbaq[2].nA);

	if(m_primitiveMode.nShading == 0)
	{
		//Flat shaded triangles use the last color set
		color1 = color2 = color3;
	}

	// clang-format off
	CDraw::PRIM_VERTEX vertices[] =
	{
		{	x1, y1, z1, color1, s[0], t[0], q[0]},
		{	x2, y2, z2, color2, s[1], t[1], q[1]},
		{	x3, y3, z3, color3, s[2], t[2], q[2]},
	};
	// clang-format on

	m_draw->AddVertices(std::begin(vertices), std::end(vertices));
}

void CGSH_Vulkan::Prim_Sprite()
{
	XYZ pos[2];
	pos[0] <<= m_vtxBuffer[1].position;
	pos[1] <<= m_vtxBuffer[0].position;

	float x1 = pos[0].GetX(), y1 = pos[0].GetY();
	float x2 = pos[1].GetX(), y2 = pos[1].GetY();
	uint32 z = pos[1].nZ;

	RGBAQ rgbaq[2];
	rgbaq[0] <<= m_vtxBuffer[1].rgbaq;
	rgbaq[1] <<= m_vtxBuffer[0].rgbaq;

	x1 -= m_primOfsX;
	x2 -= m_primOfsX;

	y1 -= m_primOfsY;
	y2 -= m_primOfsY;

	float s[2] = {0, 0};
	float t[2] = {0, 0};

	if(m_primitiveMode.nTexture)
	{
		if(m_primitiveMode.nUseUV)
		{
			UV uv[2];
			uv[0] <<= m_vtxBuffer[1].uv;
			uv[1] <<= m_vtxBuffer[0].uv;

			s[0] = uv[0].GetU() / static_cast<float>(m_texWidth);
			s[1] = uv[1].GetU() / static_cast<float>(m_texWidth);

			t[0] = uv[0].GetV() / static_cast<float>(m_texHeight);
			t[1] = uv[1].GetV() / static_cast<float>(m_texHeight);
		}
		else
		{
			ST st[2];

			st[0] <<= m_vtxBuffer[1].st;
			st[1] <<= m_vtxBuffer[0].st;

			s[0] = st[0].nS;
			s[1] = st[1].nS;

			t[0] = st[0].nT;
			t[1] = st[1].nT;
		}
	}

	auto color = MakeColor(
	    rgbaq[1].nR, rgbaq[1].nG,
	    rgbaq[1].nB, rgbaq[1].nA);

	// clang-format off
	CDraw::PRIM_VERTEX vertices[] =
	{
		{x1, y1, z, color, s[0], t[0], 1},
		{x2, y1, z, color, s[1], t[0], 1},
		{x1, y2, z, color, s[0], t[1], 1},

		{x1, y2, z, color, s[0], t[1], 1},
		{x2, y1, z, color, s[1], t[0], 1},
		{x2, y2, z, color, s[1], t[1], 1},
	};
	// clang-format on

	m_draw->AddVertices(std::begin(vertices), std::end(vertices));
}

/////////////////////////////////////////////////////////////
// Other Functions
/////////////////////////////////////////////////////////////

void CGSH_Vulkan::WriteRegisterImpl(uint8 registerId, uint64 data)
{
	CGSHandler::WriteRegisterImpl(registerId, data);

	switch(registerId)
	{
	case GS_REG_PRIM:
	{
		unsigned int newPrimitiveType = static_cast<unsigned int>(data & 0x07);
		if(newPrimitiveType != m_primitiveType)
		{
			m_draw->FlushVertices();
		}
		m_primitiveType = newPrimitiveType;
		switch(m_primitiveType)
		{
		case PRIM_POINT:
			m_vtxCount = 1;
			break;
		case PRIM_LINE:
		case PRIM_LINESTRIP:
			m_vtxCount = 2;
			break;
		case PRIM_TRIANGLE:
		case PRIM_TRIANGLESTRIP:
		case PRIM_TRIANGLEFAN:
			m_vtxCount = 3;
			break;
		case PRIM_SPRITE:
			m_vtxCount = 2;
			break;
		}
	}
	break;

	case GS_REG_XYZ2:
	case GS_REG_XYZ3:
	case GS_REG_XYZF2:
	case GS_REG_XYZF3:
		VertexKick(registerId, data);
		break;
	}
}

void CGSH_Vulkan::ProcessHostToLocalTransfer()
{
	auto bltBuf = make_convertible<BITBLTBUF>(m_nReg[GS_REG_BITBLTBUF]);
	auto trxReg = make_convertible<TRXREG>(m_nReg[GS_REG_TRXREG]);
	auto trxPos = make_convertible<TRXPOS>(m_nReg[GS_REG_TRXPOS]);

	m_transfer->Params.bufAddress = bltBuf.GetDstPtr();
	m_transfer->Params.bufWidth = bltBuf.GetDstWidth();
	m_transfer->Params.rrw = trxReg.nRRW;
	m_transfer->Params.dsax = trxPos.nDSAX;
	m_transfer->Params.dsay = trxPos.nDSAY;

	auto pipelineCaps = make_convertible<CTransfer::PIPELINE_CAPS>(0);
	pipelineCaps.dstFormat = bltBuf.nDstPsm;

	m_transfer->SetPipelineCaps(pipelineCaps);
	m_transfer->DoHostToLocalTransfer(m_xferBuffer);
}

void CGSH_Vulkan::ProcessLocalToHostTransfer()
{

}

void CGSH_Vulkan::ProcessLocalToLocalTransfer()
{

}

void CGSH_Vulkan::ProcessClutTransfer(uint32 csa, uint32)
{

}

void CGSH_Vulkan::ReadFramebuffer(uint32 width, uint32 height, void* buffer)
{

}

Framework::CBitmap CGSH_Vulkan::GetScreenshot()
{
	return Framework::CBitmap();
}
