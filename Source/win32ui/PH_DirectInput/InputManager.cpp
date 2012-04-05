#include "InputManager.h"
#include "win32/GuidUtils.h"
#include <string.h>
#include <boost/lexical_cast.hpp>

#define CONFIG_PREFIX						("input")
#define CONFIG_BINDING_TYPE					("bindingtype")

#define CONFIG_SIMPLEBINDING_PREFIX			("simplebinding")

#define CONFIG_BINDINGINFO_DEVICE			("device")
#define CONFIG_BINDINGINFO_ID				("id")

#define CONFIG_SIMULATEDAXISBINDING_PREFIX	("simulatedaxisbinding")
#define CONFIG_SIMULATEDAXISBINDING_KEY1	("key1")
#define CONFIG_SIMULATEDAXISBINDING_KEY2	("key2")

using namespace PH_DirectInput;

uint32 CInputManager::m_buttonDefaultValue[PS2::CControllerInfo::MAX_BUTTONS] =
{
	0x7FFF,
	0x7FFF,
	0x7FFF,
	0x7FFF,
	false,
	false,
	false,
	false,
	false,
	false,
	false,
	false,
	false,
	false,
	false,
	false,
	false,
	false,
};

CInputManager::CInputManager(HWND hWnd, Framework::CConfig& config)
: m_config(config)
, m_directInputManager(new Framework::DirectInput::CManager())
{
	for(unsigned int i = 0; i < PS2::CControllerInfo::MAX_BUTTONS; i++)
	{
		std::string prefBase = Framework::CConfig::MakePreferenceName(CONFIG_PREFIX, PS2::CControllerInfo::m_buttonName[i]);
		m_config.RegisterPreferenceInteger(Framework::CConfig::MakePreferenceName(prefBase, CONFIG_BINDING_TYPE).c_str(), 0);
		CSimpleBinding::RegisterPreferences(m_config, prefBase.c_str());
	}
	Load();

	m_directInputManager->RegisterInputEventHandler(std::bind(&CInputManager::OnInputEventReceived, 
		this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

	m_directInputManager->CreateKeyboard(hWnd);
	m_directInputManager->CreateJoysticks(hWnd);
}

CInputManager::~CInputManager()
{
	delete m_directInputManager;
}

void CInputManager::Load()
{
	bool hasBindings = false;
	for(unsigned int i = 0; i < PS2::CControllerInfo::MAX_BUTTONS; i++)
	{
		BINDINGTYPE bindingType = BINDING_UNBOUND;
		std::string prefBase = Framework::CConfig::MakePreferenceName(CONFIG_PREFIX, PS2::CControllerInfo::m_buttonName[i]);
		bindingType = static_cast<BINDINGTYPE>(m_config.GetPreferenceInteger((prefBase + "." + std::string(CONFIG_BINDING_TYPE)).c_str()));
		if(bindingType == BINDING_UNBOUND) continue;
		BindingPtr binding;
		switch(bindingType)
		{
		case BINDING_SIMPLE:
			binding.reset(new CSimpleBinding());
			break;
		}
		if(binding)
		{
			binding->Load(m_config, prefBase.c_str());
		}
		m_bindings[i] = binding;
		hasBindings = true;
	}
	if(!hasBindings)
	{
		AutoConfigureKeyboard();
	}
	ResetBindingValues();
}

void CInputManager::Save()
{
	for(unsigned int i = 0; i < PS2::CControllerInfo::MAX_BUTTONS; i++)
	{
		BindingPtr& binding = m_bindings[i];
		if(binding == NULL) continue;
		std::string prefBase = Framework::CConfig::MakePreferenceName(CONFIG_PREFIX, PS2::CControllerInfo::m_buttonName[i]);
		m_config.SetPreferenceInteger(Framework::CConfig::MakePreferenceName(prefBase, CONFIG_BINDING_TYPE).c_str(), binding->GetBindingType());
		binding->Save(m_config, prefBase.c_str());
	}
}

void CInputManager::AutoConfigureKeyboard()
{
	SetSimpleBinding(PS2::CControllerInfo::START,		CInputManager::BINDINGINFO(GUID_SysKeyboard, DIK_RETURN));
	SetSimpleBinding(PS2::CControllerInfo::SELECT,		CInputManager::BINDINGINFO(GUID_SysKeyboard, DIK_LSHIFT));
	SetSimpleBinding(PS2::CControllerInfo::DPAD_LEFT,	CInputManager::BINDINGINFO(GUID_SysKeyboard, DIK_LEFT));
	SetSimpleBinding(PS2::CControllerInfo::DPAD_RIGHT,	CInputManager::BINDINGINFO(GUID_SysKeyboard, DIK_RIGHT));
	SetSimpleBinding(PS2::CControllerInfo::DPAD_UP,		CInputManager::BINDINGINFO(GUID_SysKeyboard, DIK_UP));
	SetSimpleBinding(PS2::CControllerInfo::DPAD_DOWN,	CInputManager::BINDINGINFO(GUID_SysKeyboard, DIK_DOWN));
	SetSimpleBinding(PS2::CControllerInfo::SQUARE,		CInputManager::BINDINGINFO(GUID_SysKeyboard, DIK_A));
	SetSimpleBinding(PS2::CControllerInfo::CROSS,		CInputManager::BINDINGINFO(GUID_SysKeyboard, DIK_Z));
	SetSimpleBinding(PS2::CControllerInfo::TRIANGLE,	CInputManager::BINDINGINFO(GUID_SysKeyboard, DIK_S));
	SetSimpleBinding(PS2::CControllerInfo::CIRCLE,		CInputManager::BINDINGINFO(GUID_SysKeyboard, DIK_X));

	//CInputManager::GetInstance().SetSimulatedAxisBinding(CControllerInfo::ANALOG_LEFT_X,
	//	CInputManager::BINDINGINFO(GUID_SysKeyboard, DIK_LEFT),
	//	CInputManager::BINDINGINFO(GUID_SysKeyboard, DIK_RIGHT));
	//CInputManager::GetInstance().SetSimulatedAxisBinding(CControllerInfo::ANALOG_LEFT_Y,
	//	CInputManager::BINDINGINFO(GUID_SysKeyboard, DIK_UP),
	//	CInputManager::BINDINGINFO(GUID_SysKeyboard, DIK_DOWN));

	//CInputManager::GetInstance().SetSimulatedAxisBinding(CControllerInfo::ANALOG_RIGHT_X,
	//	CInputManager::BINDINGINFO(GUID_SysKeyboard, DIK_LEFT),
	//	CInputManager::BINDINGINFO(GUID_SysKeyboard, DIK_RIGHT));
	//CInputManager::GetInstance().SetSimulatedAxisBinding(CControllerInfo::ANALOG_RIGHT_Y,
	//	CInputManager::BINDINGINFO(GUID_SysKeyboard, DIK_UP),
	//	CInputManager::BINDINGINFO(GUID_SysKeyboard, DIK_DOWN));
}

const CInputManager::CBinding* CInputManager::GetBinding(PS2::CControllerInfo::BUTTON button) const
{
	if(button >= PS2::CControllerInfo::MAX_BUTTONS)
	{
		throw std::exception();
	}
	return m_bindings[button].get();
}

uint32 CInputManager::GetBindingValue(PS2::CControllerInfo::BUTTON button) const
{
	assert(button < PS2::CControllerInfo::MAX_BUTTONS);
	const auto& binding = m_bindings[button];
	if(binding)
	{
		return binding->GetValue();
	}
	else
	{
		return m_buttonDefaultValue[button];
	}
}

void CInputManager::ResetBindingValues()
{
	for(unsigned int i = 0; i < PS2::CControllerInfo::MAX_BUTTONS; i++)
	{
		auto binding = m_bindings[i];
		if(!binding) continue;
		binding->SetValue(m_buttonDefaultValue[i]);
	}
}

void CInputManager::SetSimpleBinding(PS2::CControllerInfo::BUTTON button, const BINDINGINFO& binding)
{
	if(button >= PS2::CControllerInfo::MAX_BUTTONS)
	{
		throw std::exception();
	}
	m_bindings[button].reset(new CSimpleBinding(binding.device, binding.id));
}

void CInputManager::SetSimulatedAxisBinding(PS2::CControllerInfo::BUTTON button, const BINDINGINFO& binding1, const BINDINGINFO& binding2)
{
	if(button >= PS2::CControllerInfo::MAX_BUTTONS)
	{
		throw std::exception();
	}
	m_bindings[button].reset(new CSimulatedAxisBinding(binding1, binding2));
}

void CInputManager::OnInputEventReceived(const GUID& device, uint32 id, uint32 value)
{
	for(unsigned int i = 0; i < PS2::CControllerInfo::MAX_BUTTONS; i++)
	{
		auto binding = m_bindings[i];
		if(!binding) continue;
		binding->ProcessEvent(device, id, value);
	}
}

Framework::DirectInput::CManager* CInputManager::GetDirectInputManager() const
{
	return m_directInputManager;
}

std::tstring CInputManager::GetBindingDescription(PS2::CControllerInfo::BUTTON button) const
{
	assert(button < PS2::CControllerInfo::MAX_BUTTONS);
	const auto& binding = m_bindings[button];
	if(binding)
	{
		return binding->GetDescription(m_directInputManager);
	}
	else
	{
		return _T("");
	}
}

////////////////////////////////////////////////
// SimpleBinding
////////////////////////////////////////////////

CInputManager::CSimpleBinding::CSimpleBinding(const GUID& device, uint32 id) 
: BINDINGINFO(device, id)
, m_value(0)
{

}

CInputManager::CSimpleBinding::~CSimpleBinding()
{

}

CInputManager::BINDINGTYPE CInputManager::CSimpleBinding::GetBindingType() const
{
	return BINDING_SIMPLE;
}

void CInputManager::CSimpleBinding::Save(Framework::CConfig& config, const char* buttonBase) const
{
	std::string prefBase = Framework::CConfig::MakePreferenceName(buttonBase, CONFIG_SIMPLEBINDING_PREFIX);
	config.SetPreferenceString(Framework::CConfig::MakePreferenceName(prefBase, CONFIG_BINDINGINFO_DEVICE).c_str(), boost::lexical_cast<std::string>(device).c_str());
	config.SetPreferenceInteger(Framework::CConfig::MakePreferenceName(prefBase, CONFIG_BINDINGINFO_ID).c_str(), id);
}

void CInputManager::CSimpleBinding::Load(Framework::CConfig& config, const char* buttonBase)
{
	std::string prefBase = Framework::CConfig::MakePreferenceName(buttonBase, CONFIG_SIMPLEBINDING_PREFIX);
	device = boost::lexical_cast<GUID>(config.GetPreferenceString(Framework::CConfig::MakePreferenceName(prefBase, CONFIG_BINDINGINFO_DEVICE).c_str()));
	id = config.GetPreferenceInteger(Framework::CConfig::MakePreferenceName(prefBase, CONFIG_BINDINGINFO_ID).c_str());
}

std::tstring CInputManager::CSimpleBinding::GetDescription(Framework::DirectInput::CManager* directInputManager) const
{
	DIDEVICEINSTANCE deviceInstance;
	DIDEVICEOBJECTINSTANCE objectInstance;
	if(!directInputManager->GetDeviceInfo(device, &deviceInstance))
	{
		return _T("");
	}
	if(!directInputManager->GetDeviceObjectInfo(device, id, &objectInstance))
	{
		return _T("");
	}
	return std::tstring(deviceInstance.tszInstanceName) + _T(": ") + std::tstring(objectInstance.tszName);
}

void CInputManager::CSimpleBinding::ProcessEvent(const GUID& device, uint32 id, uint32 value)
{
	if(id != BINDINGINFO::id) return;
	if(device != BINDINGINFO::device) return;
	m_value = value;
}

uint32 CInputManager::CSimpleBinding::GetValue() const
{
	return m_value;
}

void CInputManager::CSimpleBinding::SetValue(uint32 value)
{
	m_value = value;
}

void CInputManager::CSimpleBinding::RegisterPreferences(Framework::CConfig& config, const char* buttonBase)
{
	std::string prefBase = Framework::CConfig::MakePreferenceName(buttonBase, CONFIG_SIMPLEBINDING_PREFIX);
	config.RegisterPreferenceString(Framework::CConfig::MakePreferenceName(prefBase, CONFIG_BINDINGINFO_DEVICE).c_str(), boost::lexical_cast<std::string>(GUID()).c_str());
	config.RegisterPreferenceInteger(Framework::CConfig::MakePreferenceName(prefBase, CONFIG_BINDINGINFO_ID).c_str(), 0);
}

////////////////////////////////////////////////
// SimulatedAxisBinding
////////////////////////////////////////////////

CInputManager::CSimulatedAxisBinding::CSimulatedAxisBinding(const BINDINGINFO& binding1, const BINDINGINFO& binding2)
: m_key1Binding(binding1)
, m_key2Binding(binding2)
{

}

CInputManager::CSimulatedAxisBinding::~CSimulatedAxisBinding()
{

}

void CInputManager::CSimulatedAxisBinding::RegisterPreferences(Framework::CConfig& config, const char* buttonBase)
{
//    string prefBase = string(buttonBase) + "." + string(CONFIG_SIMPLEBINDING_PREFIX);
//    config.RegisterPreferenceString(
//        (prefBase + "." + string(CONFIG_SIMPLEBINDING_DEVICE)).c_str(), lexical_cast<string>(GUID()).c_str());
//    config.RegisterPreferenceInteger(
//        (prefBase + "." + string(CONFIG_SIMPLEBINDING_ID)).c_str(), 0);
}

CInputManager::BINDINGTYPE CInputManager::CSimulatedAxisBinding::GetBindingType() const
{
	return BINDING_SIMULATEDAXIS;
}

void CInputManager::CSimulatedAxisBinding::Save(Framework::CConfig& config, const char* buttonBase) const
{
	//string prefBase = CConfig::MakePreferenceName(buttonBase, CONFIG_SIMPLEBINDING_PREFIX);
	//config.SetPreferenceString(
	//    CConfig::MakePreferenceName(prefBase, CONFIG_BINDINGINFO_DEVICE).c_str(), 
	//    boost::lexical_cast<string>(device).c_str());
	//config.SetPreferenceInteger(
	//    CConfig::MakePreferenceName(prefBase, CONFIG_BINDINGINFO_ID).c_str(), 
	//    id);
}

void CInputManager::CSimulatedAxisBinding::Load(Framework::CConfig& config, const char* buttonBase)
{
	//string prefBase = CConfig::MakePreferenceName(buttonBase, CONFIG_SIMPLEBINDING_PREFIX);
	//device = boost::lexical_cast<GUID>(config.GetPreferenceString(CConfig::MakePreferenceName(prefBase, CONFIG_BINDINGINFO_DEVICE).c_str()));
	//id = config.GetPreferenceInteger(CConfig::MakePreferenceName(prefBase, CONFIG_BINDINGINFO_ID).c_str());
}

std::tstring CInputManager::CSimulatedAxisBinding::GetDescription(Framework::DirectInput::CManager* directInputManager) const
{
	//DIDEVICEINSTANCE deviceInstance;
	//DIDEVICEOBJECTINSTANCE objectInstance;
	//if(!directInputManager->GetDeviceInfo(device, &deviceInstance))
	//{
	//    return _T("");
	//}
	//if(!directInputManager->GetDeviceObjectInfo(device, id, &objectInstance))
	//{
	//    return _T("");
	//}
	//return tstring(deviceInstance.tszInstanceName) + _T(": ") + tstring(objectInstance.tszName);
	return std::tstring(_T("pwned!"));
}

uint32 CInputManager::CSimulatedAxisBinding::GetValue() const
{
	uint32 value = 0;
	if(m_key1State && m_key2State)
	{
		value = 0;
	}
	if(m_key1State)
	{
		value = 0x7FFF;
	}
	else if(m_key2State)
	{
		value = 0x8000;
	}
	return value;
}

void CInputManager::CSimulatedAxisBinding::SetValue(uint32 value)
{
	m_key1State = 0;
	m_key2State = 0;
}

void CInputManager::CSimulatedAxisBinding::ProcessEvent(const GUID& device, uint32 id, uint32 value)
{
	if(id == m_key1Binding.id && device == m_key1Binding.device)
	{
		m_key1State = value;
	}
	else if(id == m_key2Binding.id && device == m_key2Binding.device)
	{
		m_key2State = value;
	}
}
