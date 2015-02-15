<?xml version='1.0' encoding='UTF-8'?>
<Project Type="Project" LVVersion="10008000">
	<Property Name="varPersistentID:{136FBB01-D107-4BE5-A384-D4BFAD4F383C}" Type="Ref">/My Computer/example.lvlib/some_struct</Property>
	<Property Name="varPersistentID:{28F126BF-F781-4D7C-997C-F1EB3BFD178B}" Type="Ref">/My Computer/example.lvlib/some_indicator</Property>
	<Property Name="varPersistentID:{4089568F-CE4D-4CFE-B5B9-DDD4C34C46D1}" Type="Ref">/My Computer/example.lvlib/some_control</Property>
	<Property Name="varPersistentID:{44D862AB-4EAA-47BA-8232-ACDD0189A9B5}" Type="Ref">/My Computer/example.lvlib/some_array</Property>
	<Property Name="varPersistentID:{650D14BC-6AC4-4B8E-9E01-6347E86E3C0B}" Type="Ref">/My Computer/example.lvlib/some_bool</Property>
	<Property Name="varPersistentID:{D85B330D-A334-4CE0-A900-683E2B16A436}" Type="Ref">/My Computer/example.lvlib/some_string</Property>
	<Item Name="My Computer" Type="My Computer">
		<Property Name="server.app.propertiesEnabled" Type="Bool">true</Property>
		<Property Name="server.control.propertiesEnabled" Type="Bool">true</Property>
		<Property Name="server.tcp.enabled" Type="Bool">false</Property>
		<Property Name="server.tcp.port" Type="Int">0</Property>
		<Property Name="server.tcp.serviceName" Type="Str">My Computer/VI Server</Property>
		<Property Name="server.tcp.serviceName.default" Type="Str">My Computer/VI Server</Property>
		<Property Name="server.vi.callsEnabled" Type="Bool">true</Property>
		<Property Name="server.vi.propertiesEnabled" Type="Bool">true</Property>
		<Property Name="specify.custom.address" Type="Bool">false</Property>
		<Item Name="example.lvlib" Type="Library" URL="../example.lvlib"/>
		<Item Name="example.vi" Type="VI" URL="../example.vi"/>
		<Item Name="Dependencies" Type="Dependencies"/>
		<Item Name="Build Specifications" Type="Build"/>
	</Item>
</Project>
